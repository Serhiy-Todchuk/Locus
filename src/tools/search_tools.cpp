#include "tools/search_tools.h"
#include "tools/shared.h"

#include "core/workspace_services.h"
#include "embedding_worker.h"
#include "glob_match.h"
#include "index_query.h"
#include "reranker.h"
#include "workspace.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace locus {

namespace fs = std::filesystem;

using tools::error_result;

// -- SearchTool (unified face) ----------------------------------------------
// Dispatches on `mode` to one of the four implementations below. Keeps the
// exposed tool catalog small (S3.L) without duplicating search logic.

ToolResult SearchTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string mode = call.args.value("mode", "text");

    // Rewrite args so the delegate sees exactly what it expects.
    ToolCall sub = call;

    if (mode == "text") {
        SearchTextTool impl;
        return impl.execute(sub, ws);
    }
    if (mode == "symbols") {
        // SearchSymbolsTool expects `name` rather than `query` — translate.
        if (!sub.args.contains("name") && sub.args.contains("query"))
            sub.args["name"] = sub.args["query"];
        SearchSymbolsTool impl;
        return impl.execute(sub, ws);
    }
    if (mode == "semantic") {
        SearchSemanticTool impl;
        return impl.execute(sub, ws);
    }
    if (mode == "hybrid") {
        SearchHybridTool impl;
        return impl.execute(sub, ws);
    }
    if (mode == "regex") {
        SearchRegexTool impl;
        return impl.execute(sub, ws);
    }
    if (mode == "ast") {
        return error_result("Error: mode '" + mode + "' is not yet implemented "
                            "(planned for M4).");
    }
    return error_result("Error: unknown search mode '" + mode +
                        "'. Supported: text, regex, symbols, semantic, hybrid.");
}

// -- SearchTextTool ---------------------------------------------------------

ToolResult SearchTextTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string query = call.args.value("query", "");
    int max_results = call.args.value("max_results", 20);

    if (query.empty())
        return error_result("Error: 'query' parameter is required");

    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    SearchOptions opts;
    opts.max_results = max_results;

    auto results = idx->search_text(query, opts);

    std::ostringstream content;
    content << results.size() << " results for \"" << query << "\"\n";
    for (auto& r : results) {
        content << "  " << r.path;
        if (r.line > 0) content << ":" << r.line;
        content << " (score " << r.score << ")\n";
        if (!r.snippet.empty())
            content << "    " << r.snippet << "\n";
    }

    std::string result = content.str();
    spdlog::trace("search_text: '{}' -> {} results", query, results.size());
    return {true, result, result};
}

// -- SearchSymbolsTool ------------------------------------------------------

ToolResult SearchSymbolsTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string name_query = call.args.value("name", "");
    std::string kind     = call.args.value("kind", "");
    std::string language = call.args.value("language", "");

    if (name_query.empty())
        return error_result("Error: 'name' parameter is required");

    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    auto results = idx->search_symbols(name_query, kind, language);

    std::ostringstream content;
    content << results.size() << " symbols matching \"" << name_query << "\"\n";
    for (auto& s : results) {
        content << "  " << s.kind << " " << s.name;
        if (!s.signature.empty()) content << s.signature;
        content << "  " << s.path << ":" << s.line_start;
        if (s.line_end > s.line_start)
            content << "-" << s.line_end;
        if (!s.language.empty())
            content << "  [" << s.language << "]";
        content << "\n";
    }

    std::string result = content.str();
    spdlog::trace("search_symbols: '{}' -> {} results", name_query, results.size());
    return {true, result, result};
}

// -- SearchSemanticTool -----------------------------------------------------

namespace {

// If a reranker is wired up on the workspace, fetch top_k bi-encoder
// candidates and rerank them down to max_results. Otherwise just trim
// `results` to max_results in place (the bi-encoder ordering is preserved).
//
// Mutates `results` to reflect the reranked order; rewrites .score to the
// reranker logit when reranking happens.
void apply_reranker(std::vector<SearchResult>& results,
                    IWorkspaceServices& ws,
                    const std::string& query,
                    int max_results)
{
    auto* rr = ws.reranker();
    if (!rr || results.size() <= 1) {
        if (static_cast<int>(results.size()) > max_results)
            results.resize(static_cast<size_t>(max_results));
        return;
    }

    std::vector<std::string> passages;
    passages.reserve(results.size());
    for (const auto& r : results) passages.push_back(r.snippet);

    std::vector<float> scores;
    try {
        scores = rr->score_batch(query, passages);
    } catch (const std::exception& e) {
        spdlog::warn("Reranker failed, falling back to bi-encoder order: {}", e.what());
        if (static_cast<int>(results.size()) > max_results)
            results.resize(static_cast<size_t>(max_results));
        return;
    }

    std::vector<size_t> idx(results.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(),
              [&](size_t a, size_t b) { return scores[a] > scores[b]; });

    std::vector<SearchResult> reranked;
    reranked.reserve(std::min<size_t>(idx.size(), static_cast<size_t>(max_results)));
    for (size_t i = 0; i < idx.size() && static_cast<int>(reranked.size()) < max_results; ++i) {
        SearchResult r = results[idx[i]];
        r.score = static_cast<double>(scores[idx[i]]);
        reranked.push_back(std::move(r));
    }
    results = std::move(reranked);
}

// Returns the workspace's configured reranker_top_k, or `fallback` if the
// services interface doesn't expose a Workspace (e.g. in tests).
int reranker_top_k_for(IWorkspaceServices& ws, int fallback)
{
    if (auto* w = ws.workspace()) return w->config().reranker_top_k;
    return fallback;
}

// If the embedding worker still has chunks in flight, returns a one-line
// note to prepend to the tool result so both the user and the LLM know the
// vector index is incomplete. Empty string when the worker is idle - in
// which case the search results are over the full corpus.
//
// Without this, a query fired during cold-start indexing would silently
// return a top-K from the partial subset, indistinguishable from a real
// zero-match query against a fully-indexed workspace.
std::string indexing_progress_note(IWorkspaceServices& ws)
{
    auto* emb = ws.embedder();
    if (!emb) return {};
    auto s = emb->stats();
    if (s.total <= 0 || s.done >= s.total) return {};
    int pct = static_cast<int>((100LL * s.done) / s.total);
    std::ostringstream o;
    o << "Note: indexing in progress (" << s.done << "/" << s.total
      << " chunks embedded, " << pct
      << "%). Semantic results below are partial - retry once indexing "
         "completes for full coverage.\n\n";
    return o.str();
}

} // namespace

ToolResult SearchSemanticTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    auto* emb = ws.embedder();
    if (!emb)
        return error_result("Error: semantic search is not enabled. "
                            "Enable it in Settings > Index > Semantic Search.");
    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    std::string query = call.args.value("query", "");
    if (query.empty())
        return error_result("Error: 'query' parameter is required");

    int max_results = call.args.value("max_results", 10);

    auto embedding = emb->embed_query(query);

    SearchOptions opts;
    // Pull a wider candidate set when a reranker is active; the rerank step
    // will trim back down to max_results.
    bool use_rr = ws.reranker() != nullptr;
    opts.max_results = use_rr
        ? std::max(max_results, reranker_top_k_for(ws, 50))
        : max_results;
    auto results = idx->search_semantic(embedding, opts);

    apply_reranker(results, ws, query, max_results);

    std::string progress = indexing_progress_note(ws);

    if (results.empty()) {
        std::string body = progress + "No semantic matches found.";
        return {true, body, body};
    }

    std::ostringstream out;
    out << progress
        << results.size() << " semantic matches"
        << (use_rr ? " (reranked):\n" : ":\n");
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        out << "\n[" << (i + 1) << "] " << r.path << ":" << r.line
            << (use_rr ? " (rerank: " : " (similarity: ")
            << std::fixed << std::setprecision(3) << r.score << ")\n";
        out << r.snippet << "\n";
    }

    std::string result = out.str();
    return {true, result, result};
}

// -- SearchRegexTool --------------------------------------------------------

namespace {

// 1 MB per-file cap keeps `std::regex` latency bounded on pathological
// generated files that still slip past the indexer's size filter.
constexpr size_t k_regex_file_read_cap = 1 * 1024 * 1024;

// Build 1-based line-start offsets for fast position -> line mapping.
std::vector<size_t> compute_line_starts(const std::string& content)
{
    std::vector<size_t> starts;
    starts.reserve(content.size() / 40 + 1);
    starts.push_back(0);
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') starts.push_back(i + 1);
    }
    return starts;
}

// Extract a snippet around `line` (1-based) with `context_lines` on each side,
// trimmed and length-capped. Mirrors index_query.cpp's helper.
std::string extract_regex_snippet(const std::string& content,
                                  const std::vector<size_t>& line_starts,
                                  int line, int context_lines)
{
    if (line <= 0 || line_starts.empty()) return {};
    int start_line = std::max(1, line - context_lines);
    int end_line   = std::min(static_cast<int>(line_starts.size()),
                              line + context_lines);
    size_t s = line_starts[start_line - 1];
    size_t e = (end_line < static_cast<int>(line_starts.size()))
                   ? line_starts[end_line]
                   : content.size();
    std::string snippet = content.substr(s, e - s);
    while (!snippet.empty() &&
           (snippet.back() == '\n' || snippet.back() == '\r'))
        snippet.pop_back();
    if (snippet.size() > 500) {
        snippet.resize(500);
        snippet += "...";
    }
    return snippet;
}

} // namespace

ToolResult SearchRegexTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string pattern = call.args.value("pattern", "");
    if (pattern.empty())
        return error_result("Error: 'pattern' parameter is required");

    std::string path_glob    = call.args.value("path_glob", "");
    bool        case_sens    = call.args.value("case_sensitive", true);
    int         max_results  = call.args.value("max_results", 50);
    if (max_results <= 0) max_results = 50;

    auto* idx = ws.index();
    if (!idx) return error_result("Error: workspace index not available");

    std::regex re;
    try {
        auto flags = std::regex::ECMAScript;
        if (!case_sens) flags |= std::regex::icase;
        re.assign(pattern, flags);
    } catch (const std::regex_error& e) {
        return error_result(std::string("Error: invalid regex: ") + e.what());
    }

    // Enumerate every indexed file. `list_directory("", huge_depth)` emits
    // files (never folded into parent directories) because nothing exceeds
    // the depth cap. The indexer's exclude patterns are already baked into
    // the files table, so no re-filtering here.
    auto files = idx->list_directory("", 1'000'000);

    struct Match {
        std::string path;
        int         line = 0;
        std::string snippet;
    };
    std::vector<Match> matches;
    matches.reserve(max_results);

    int files_searched = 0;
    int files_with_matches = 0;
    bool truncated = false;

    for (auto& fe : files) {
        if (fe.is_directory) continue;
        if (fe.is_binary)    continue;

        if (!path_glob.empty() && !glob_match(path_glob, fe.path)) continue;

        fs::path abs = ws.root() / fe.path;
        std::ifstream f(abs, std::ios::binary);
        if (!f.is_open()) continue;

        std::string content{ std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>() };
        if (content.empty()) continue;
        if (content.size() > k_regex_file_read_cap)
            content.resize(k_regex_file_read_cap);

        ++files_searched;

        auto line_starts = compute_line_starts(content);

        bool file_had_match = false;

        auto it  = std::sregex_iterator(content.begin(), content.end(), re);
        auto end = std::sregex_iterator();
        for (; it != end; ++it) {
            if (static_cast<int>(matches.size()) >= max_results) {
                truncated = true;
                break;
            }
            size_t pos = static_cast<size_t>(it->position());
            auto   lit = std::upper_bound(line_starts.begin(),
                                          line_starts.end(), pos);
            int    line = static_cast<int>(lit - line_starts.begin());

            Match m;
            m.path    = fe.path;
            m.line    = line;
            m.snippet = extract_regex_snippet(content, line_starts, line, 1);
            matches.push_back(std::move(m));
            file_had_match = true;
        }
        if (file_had_match) ++files_with_matches;
        if (truncated) break;
    }

    std::ostringstream out;
    out << matches.size() << " match" << (matches.size() == 1 ? "" : "es")
        << " for /" << pattern << "/" << (case_sens ? "" : "i")
        << " in " << files_with_matches << " file"
        << (files_with_matches == 1 ? "" : "s")
        << " (searched " << files_searched << ")";
    if (truncated) out << " [truncated at max_results]";
    out << "\n";

    for (auto& m : matches) {
        out << "  " << m.path << ":" << m.line << "\n";
        std::istringstream is(m.snippet);
        std::string        ln;
        while (std::getline(is, ln)) {
            out << "    " << ln << "\n";
        }
    }

    std::string result = out.str();
    spdlog::trace("search_regex: /{}/ -> {} matches across {} files",
                  pattern, matches.size(), files_with_matches);
    return {true, result, result};
}

// -- SearchHybridTool -------------------------------------------------------

ToolResult SearchHybridTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    auto* emb = ws.embedder();
    if (!emb)
        return error_result("Error: semantic search is not enabled. "
                            "Enable it in Settings > Index > Semantic Search.");
    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    std::string query = call.args.value("query", "");
    if (query.empty())
        return error_result("Error: 'query' parameter is required");

    int max_results = call.args.value("max_results", 10);

    auto embedding = emb->embed_query(query);

    SearchOptions opts;
    bool use_rr = ws.reranker() != nullptr;
    opts.max_results = use_rr
        ? std::max(max_results, reranker_top_k_for(ws, 50))
        : max_results;
    auto results = idx->search_hybrid(query, embedding, opts);

    apply_reranker(results, ws, query, max_results);

    std::string progress = indexing_progress_note(ws);

    if (results.empty()) {
        std::string body = progress + "No matches found.";
        return {true, body, body};
    }

    std::ostringstream out;
    out << progress
        << results.size() << " hybrid matches (BM25 + semantic"
        << (use_rr ? " + rerank):\n" : "):\n");
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        out << "\n[" << (i + 1) << "] " << r.path << ":" << r.line
            << " (score: " << std::fixed << std::setprecision(4) << r.score << ")\n";
        out << r.snippet << "\n";
    }

    std::string result = out.str();
    return {true, result, result};
}

} // namespace locus
