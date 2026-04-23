#include "tools/search_tools.h"
#include "tools/shared.h"

#include "core/workspace_services.h"
#include "embedding_worker.h"
#include "index_query.h"

#include <spdlog/spdlog.h>

#include <iomanip>
#include <sstream>
#include <string>

namespace locus {

using tools::error_result;

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
    opts.max_results = max_results;
    auto results = idx->search_semantic(embedding, opts);

    if (results.empty())
        return {true, "No semantic matches found.", "No semantic matches found."};

    std::ostringstream out;
    out << results.size() << " semantic matches:\n";
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        out << "\n[" << (i + 1) << "] " << r.path << ":" << r.line
            << " (similarity: " << std::fixed << std::setprecision(3) << r.score << ")\n";
        out << r.snippet << "\n";
    }

    std::string result = out.str();
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
    opts.max_results = max_results;
    auto results = idx->search_hybrid(query, embedding, opts);

    if (results.empty())
        return {true, "No matches found.", "No matches found."};

    std::ostringstream out;
    out << results.size() << " hybrid matches (BM25 + semantic):\n";
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
