#include "tools/search_tools.h"
#include "tools/shared.h"

#include "core/workspace_services.h"
#include "embedding_worker.h"
#include "index/glob_match.h"
#include "index/index_query.h"
#include "index/tree_sitter_registry.h"
#include "reranker.h"
#include "workspace.h"

#include <spdlog/spdlog.h>
#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdint>
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
        SearchAstTool impl;
        return impl.execute(sub, ws);
    }
    return error_result("Error: unknown search mode '" + mode +
                        "'. Supported: text, regex, symbols, ast, semantic, hybrid.");
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

// -- SearchAstTool ----------------------------------------------------------

namespace {

constexpr size_t k_ast_file_read_cap = 1 * 1024 * 1024;

const char* describe_query_error(TSQueryError e)
{
    switch (e) {
        case TSQueryErrorNone:      return "none";
        case TSQueryErrorSyntax:    return "syntax";
        case TSQueryErrorNodeType:  return "unknown node type";
        case TSQueryErrorField:     return "unknown field name";
        case TSQueryErrorCapture:   return "unknown capture";
        case TSQueryErrorStructure: return "structure";
        case TSQueryErrorLanguage:  return "language";
    }
    return "unknown";
}

// Single-line preview of the captured node's source. Newlines collapse to
// spaces so the LLM gets a useful identifier without paging through a class
// body when the capture is a whole class_declaration node.
std::string inline_match_text(const std::string& content, uint32_t start, uint32_t end)
{
    if (start >= content.size() || end > content.size() || start >= end) return {};
    std::string text = content.substr(start, end - start);
    if (text.size() > 200) {
        text.resize(200);
        text += "...";
    }
    for (char& c : text) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return text;
}

// Tree-sitter exposes `#eq?` / `#match?` / `#any-of?` predicates in a
// query's source but does NOT enforce them inside `ts_query_cursor_next_match`
// -- the consumer is expected to walk the predicate steps and reject matches
// that fail the constraints. We support the common subset: eq?, not-eq?,
// match?, not-match?, any-of?, not-any-of?. Unknown predicates accept the
// match (forward-compatible with future Tree-sitter additions).
bool match_passes_predicates(TSQuery* query, const TSQueryMatch& m,
                             const std::string& source)
{
    uint32_t step_count = 0;
    const TSQueryPredicateStep* steps =
        ts_query_predicates_for_pattern(query, m.pattern_index, &step_count);
    if (step_count == 0) return true;

    auto capture_text = [&](uint32_t capture_id) -> std::string {
        for (uint16_t i = 0; i < m.capture_count; ++i) {
            if (m.captures[i].index == capture_id) {
                uint32_t s = ts_node_start_byte(m.captures[i].node);
                uint32_t e = ts_node_end_byte(m.captures[i].node);
                if (s < source.size() && e <= source.size() && s < e)
                    return source.substr(s, e - s);
                return {};
            }
        }
        return {};
    };
    auto string_value = [&](uint32_t string_id) -> std::string {
        uint32_t len = 0;
        const char* p = ts_query_string_value_for_id(query, string_id, &len);
        return std::string(p ? p : "", len);
    };

    uint32_t i = 0;
    while (i < step_count) {
        if (steps[i].type != TSQueryPredicateStepTypeString) {
            // Malformed predicate -- skip to Done and continue.
            while (i < step_count && steps[i].type != TSQueryPredicateStepTypeDone) ++i;
            if (i < step_count) ++i;
            continue;
        }

        std::string name = string_value(steps[i].value_id);
        ++i;

        std::vector<std::string> args;
        while (i < step_count && steps[i].type != TSQueryPredicateStepTypeDone) {
            if (steps[i].type == TSQueryPredicateStepTypeCapture)
                args.push_back(capture_text(steps[i].value_id));
            else
                args.push_back(string_value(steps[i].value_id));
            ++i;
        }
        if (i < step_count) ++i;  // skip Done

        if ((name == "eq?" || name == "not-eq?") && args.size() == 2) {
            bool eq = (args[0] == args[1]);
            if (name == "eq?"     && !eq) return false;
            if (name == "not-eq?" &&  eq) return false;
        } else if ((name == "match?" || name == "not-match?") && args.size() == 2) {
            try {
                std::regex re(args[1], std::regex::ECMAScript);
                bool ok = std::regex_search(args[0], re);
                if (name == "match?"     && !ok) return false;
                if (name == "not-match?" &&  ok) return false;
            } catch (const std::regex_error&) {
                // Bad regex -- treat as no-op so the match isn't lost.
            }
        } else if ((name == "any-of?" || name == "not-any-of?") && args.size() >= 2) {
            bool found = false;
            for (size_t k = 1; k < args.size(); ++k) {
                if (args[0] == args[k]) { found = true; break; }
            }
            if (name == "any-of?"     && !found) return false;
            if (name == "not-any-of?" &&  found) return false;
        }
        // Unknown predicate: accept.
    }
    return true;
}

} // namespace

ToolResult SearchAstTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string language       = call.args.value("language", "");
    std::string query_src      = call.args.value("query", "");
    std::string path_glob      = call.args.value("path_glob", "");
    std::string capture_filter = call.args.value("capture", "");
    int         max_results    = call.args.value("max_results", 50);
    if (max_results <= 0) max_results = 50;

    if (language.empty())
        return error_result("Error: 'language' parameter is required");
    if (query_src.empty())
        return error_result("Error: 'query' parameter is required");

    auto* idx = ws.index();
    if (!idx) return error_result("Error: workspace index not available");

    // Fresh registry per call -- ts_parser_new() is cheap and avoids any
    // shared-state coupling with the indexer's parser.
    TreeSitterRegistry reg;
    const TSLanguage* ts_lang = reg.language_for_name(language);
    if (!ts_lang)
        return error_result("Error: unsupported language '" + language +
            "'. Supported: c, cpp, python, javascript, typescript, "
            "go, rust, java, csharp.");

    uint32_t      err_offset = 0;
    TSQueryError  err_type   = TSQueryErrorNone;
    TSQuery* query = ts_query_new(ts_lang, query_src.c_str(),
                                   static_cast<uint32_t>(query_src.size()),
                                   &err_offset, &err_type);
    if (!query) {
        std::ostringstream o;
        o << "Error: invalid Tree-sitter query ("
          << describe_query_error(err_type)
          << " at byte " << err_offset << ")";
        return error_result(o.str());
    }

    ts_parser_set_language(reg.parser(), ts_lang);
    TSQueryCursor* cursor = ts_query_cursor_new();

    struct Match {
        std::string path;
        int         line = 0;
        std::string capture_name;
        std::string match_text;
        std::string snippet;
    };
    std::vector<Match> matches;

    auto files = idx->list_directory("", 1'000'000);

    int  files_searched     = 0;
    int  files_with_matches = 0;
    bool truncated          = false;

    for (auto& fe : files) {
        if (truncated) break;
        if (fe.is_directory) continue;
        if (fe.is_binary)    continue;
        if (fe.language != language) continue;
        if (!path_glob.empty() && !glob_match(path_glob, fe.path)) continue;

        fs::path abs = ws.root() / fe.path;
        std::ifstream f(abs, std::ios::binary);
        if (!f.is_open()) continue;

        std::string content{ std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>() };
        if (content.empty()) continue;
        if (content.size() > k_ast_file_read_cap)
            content.resize(k_ast_file_read_cap);

        TSTree* tree = ts_parser_parse_string(reg.parser(), nullptr,
                                              content.c_str(),
                                              static_cast<uint32_t>(content.size()));
        if (!tree) continue;

        ++files_searched;

        ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));

        auto line_starts = compute_line_starts(content);

        TSQueryMatch m;
        bool file_had_match = false;
        while (ts_query_cursor_next_match(cursor, &m)) {
            if (!match_passes_predicates(query, m, content)) continue;
            for (uint16_t i = 0; i < m.capture_count; ++i) {
                const TSQueryCapture& cap = m.captures[i];

                uint32_t name_len = 0;
                const char* name_ptr =
                    ts_query_capture_name_for_id(query, cap.index, &name_len);
                std::string cap_name(name_ptr ? name_ptr : "", name_len);
                if (!capture_filter.empty() && cap_name != capture_filter) continue;

                if (static_cast<int>(matches.size()) >= max_results) {
                    truncated = true;
                    break;
                }

                uint32_t s = ts_node_start_byte(cap.node);
                uint32_t e = ts_node_end_byte(cap.node);
                TSPoint  sp = ts_node_start_point(cap.node);
                int      line = static_cast<int>(sp.row) + 1;

                Match match;
                match.path         = fe.path;
                match.line         = line;
                match.capture_name = std::move(cap_name);
                match.match_text   = inline_match_text(content, s, e);
                match.snippet      = extract_regex_snippet(content, line_starts, line, 1);
                matches.push_back(std::move(match));
                file_had_match = true;
            }
            if (truncated) break;
        }

        ts_tree_delete(tree);
        if (file_had_match) ++files_with_matches;
    }

    ts_query_cursor_delete(cursor);
    ts_query_delete(query);

    std::ostringstream out;
    out << matches.size() << " match" << (matches.size() == 1 ? "" : "es")
        << " for [" << language << "] AST query in "
        << files_with_matches << " file"
        << (files_with_matches == 1 ? "" : "s")
        << " (searched " << files_searched << ")";
    if (truncated) out << " [truncated at max_results]";
    out << "\n";

    for (auto& m : matches) {
        out << "  " << m.path << ":" << m.line;
        if (!m.capture_name.empty())
            out << "  @" << m.capture_name;
        if (!m.match_text.empty())
            out << "  " << m.match_text;
        out << "\n";
        std::istringstream is(m.snippet);
        std::string ln;
        while (std::getline(is, ln)) {
            out << "    " << ln << "\n";
        }
    }

    std::string result = out.str();
    spdlog::trace("search_ast: [{}] -> {} matches across {} files",
                  language, matches.size(), files_with_matches);
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
