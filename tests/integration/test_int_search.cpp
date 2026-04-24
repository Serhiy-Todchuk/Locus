#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace locus::integration;

namespace {

// Returns true if any observed tool result for a `search` call contains `needle`
// anywhere in its display text. Search results are formatted as a list of
// path:line lines so grepping the display is sufficient for a coarse match.
bool search_result_mentions(const PromptResult& r, std::string_view needle)
{
    for (const auto& call : r.tool_calls) {
        if (call.tool_name != "search") continue;
        for (const auto& res : r.tool_results) {
            if (res.call_id == call.id &&
                res.display.find(needle) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

std::string search_mode(const PromptResult& r)
{
    const auto* call = r.find_tool_call("search");
    if (!call) return "";
    auto it = call->args.find("mode");
    if (it == call->args.end() || !it->is_string()) return "text";  // default
    return it->get<std::string>();
}

} // namespace

TEST_CASE("text search finds literal identifier", "[integration][llm][search]")
{
    auto& h = harness();
    PromptResult r = h.prompt(
        "Use the search tool in text mode to find literal occurrences of the "
        "identifier `register_builtin_tools` in this codebase, then report which "
        "file defines it.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("search"));
    // Accept text (preferred) or hybrid (fallback LLM choice).
    std::string mode = search_mode(r);
    REQUIRE((mode == "text" || mode == "hybrid"));
    REQUIRE(search_result_mentions(r, "tools/tools.cpp"));
}

TEST_CASE("symbol search locates a class definition", "[integration][llm][search]")
{
    auto& h = harness();
    PromptResult r = h.prompt(
        "Use the search tool to locate where the C++ class `AgentCore` is "
        "defined. Tell me the file path.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.tool_called("search"));
    // Any mode is acceptable — we assert on whether the right file comes back.
    REQUIRE((search_result_mentions(r, "agent_core.h") ||
             search_result_mentions(r, "agent_core.cpp")));
}

TEST_CASE("semantic / hybrid search finds embedding code", "[integration][llm][search][semantic]")
{
    auto& h = harness();
    // Make sure embeddings are caught up before running semantic query.
    h.wait_for_embedding_idle();

    PromptResult r = h.prompt(
        "Use the search tool in semantic mode to find code responsible for "
        "running chunk embeddings into a vector database. Report the top file.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.tool_called("search"));

    std::string mode = search_mode(r);
    INFO("mode=" << mode);
    // Accept either semantic or hybrid — both should surface the embedding code.
    bool plausible =
        search_result_mentions(r, "embedding_worker") ||
        search_result_mentions(r, "embedder") ||
        search_result_mentions(r, "chunker");
    REQUIRE(plausible);
}

TEST_CASE("hybrid search returns RRF-merged results", "[integration][llm][search]")
{
    auto& h = harness();
    h.wait_for_embedding_idle();

    PromptResult r = h.prompt(
        "Use the search tool in hybrid mode to find code related to atomic "
        "file writes using a temp file and rename. Which file implements that?");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.tool_called("search"));
    REQUIRE(search_result_mentions(r, "file_tools"));
}

// -- Regex mode (S4.P) ------------------------------------------------------

TEST_CASE("regex search finds an exact punctuation-preserving identifier",
          "[integration][llm][search][regex]")
{
    auto& h = harness();
    PromptResult r = h.prompt(
        "Use the search tool in regex mode with pattern `SQLITE_TRANSIENT` "
        "to find every occurrence of that exact constant name. Tell me which "
        "files contain it.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("search"));

    // The LLM was told to use regex mode explicitly. Accept regex only —
    // the whole point is that regex preserves casing / punctuation that FTS5
    // would tokenize away.
    REQUIRE(search_mode(r) == "regex");

    // SQLITE_TRANSIENT lives in indexer.cpp, index_query.cpp, embedding_worker.cpp.
    // Any one of those surfacing is enough — the LLM's report might cite just one.
    bool mentioned =
        search_result_mentions(r, "indexer.cpp") ||
        search_result_mentions(r, "index_query.cpp") ||
        search_result_mentions(r, "embedding_worker.cpp");
    REQUIRE(mentioned);
}

TEST_CASE("regex search matches a pattern with regex metacharacters",
          "[integration][llm][search][regex]")
{
    auto& h = harness();
    PromptResult r = h.prompt(
        "Use the search tool in regex mode to find calls to sqlite3_reset "
        "on variables whose name starts with `stmt_`. Use the pattern "
        "`sqlite3_reset\\(stmt_\\w+\\)`. Report a file that matches.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.tool_called("search"));
    REQUIRE(search_mode(r) == "regex");

    // `sqlite3_reset(stmt_...)` appears in indexer.cpp / index_query.cpp /
    // embedding_worker.cpp.
    bool mentioned =
        search_result_mentions(r, "indexer.cpp") ||
        search_result_mentions(r, "index_query.cpp") ||
        search_result_mentions(r, "embedding_worker.cpp");
    REQUIRE(mentioned);
}
