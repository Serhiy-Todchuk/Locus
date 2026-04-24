#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace locus::integration;

TEST_CASE("agent routes ask_user through the approval gate", "[integration][llm][ask_user]")
{
    auto& h = harness();

    // Stage the answer the harness will supply when the LLM invokes ask_user.
    h.frontend().queue_ask_user_response("42");

    PromptResult r = h.prompt(
        "Use the ask_user tool to ask the user for their favourite number. "
        "Then tell me exactly what number they answered with.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("ask_user"));

    // The tool's result display should include the staged answer.
    const auto* res = r.find_result_for("ask_user");
    REQUIRE(res != nullptr);
    REQUIRE(res->display.find("42") != std::string::npos);

    // And the LLM's final answer should mention the number the harness fed back.
    REQUIRE(r.tokens.find("42") != std::string::npos);
}
