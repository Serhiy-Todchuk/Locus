#include "harness_fixture.h"

#include "index/indexer.h"

#include <catch2/catch_test_macros.hpp>

using namespace locus::integration;

TEST_CASE("LLM is reachable and workspace is indexed", "[integration][llm][smoke]")
{
    // First access triggers construction; a reachable LLM is the prerequisite.
    auto& h = harness();

    REQUIRE_FALSE(h.model_id().empty());
    REQUIRE(std::filesystem::is_directory(h.workspace_root()));
    REQUIRE(h.workspace().indexer().stats().files_total > 0);
}

TEST_CASE("agent can read a known file via tool", "[integration][llm][smoke]")
{
    auto& h = harness();

    PromptResult r = h.prompt(
        "Use the read_file tool to read CLAUDE.md (at the workspace root) and tell "
        "me one sentence about what Locus is, quoting from the file.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());

    // The LLM must have invoked read_file somewhere in the turn. We don't
    // constrain which path flavor (CLAUDE.md, ./CLAUDE.md, CLAUDE.md with
    // slashes) — just that a read_file call happened and produced a non-empty
    // result.
    REQUIRE(r.tool_called("read_file"));
    const auto* res = r.find_result_for("read_file");
    REQUIRE(res != nullptr);
    REQUIRE_FALSE(res->display.empty());

    // The final answer should reference something from CLAUDE.md's vocabulary.
    // Keep the assertion loose — any of these words indicates the LLM read the
    // file rather than hallucinating.
    bool referenced =
        r.tokens.find("Locus") != std::string::npos ||
        r.tokens.find("locus") != std::string::npos ||
        r.tokens.find("workspace") != std::string::npos ||
        r.tokens.find("agent") != std::string::npos;
    REQUIRE(referenced);
}
