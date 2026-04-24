#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace locus::integration;

TEST_CASE("/help slash command runs without LLM round-trip", "[integration][slash]")
{
    auto& h = harness();

    // Slash commands are intercepted before any LLM call. We still wait on
    // turn_complete because the dispatcher uses the same completion path.
    PromptResult r = h.prompt("/help");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_calls.empty());  // /help doesn't dispatch a tool
    REQUIRE_FALSE(r.tokens.empty());
}

TEST_CASE("/read_file slash command directly invokes the read_file tool",
          "[integration][slash]")
{
    auto& h = harness();

    PromptResult r = h.prompt("/read_file CLAUDE.md");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    // Slash-invoked tools don't flow through on_tool_call_pending (they
    // bypass the approval gate). They DO emit textual output via the slash
    // dispatcher's on_output callback, which arrives as tokens.
    REQUIRE_FALSE(r.tokens.empty());
}
