#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace locus::integration;

TEST_CASE("agent runs a shell command", "[integration][llm][shell]")
{
    auto& h = harness();

    PromptResult r = h.prompt(
        "Use the run_command tool to execute the shell command "
        "`cmd /c echo hello-locus-integration` and tell me exactly what it "
        "printed.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("run_command"));

    const auto* res = r.find_result_for("run_command");
    REQUIRE(res != nullptr);
    REQUIRE(res->display.find("hello-locus-integration") != std::string::npos);
}

// run_command output filter: default smart-truncate. The agent runs a
// verbose command (300 numbered lines) and the tool result must contain the
// "[... K lines elided ...]" marker the helper inserts when total > 2 * 50.
// This is a no-LLM-decision-on-filter-arg test: the LLM only has to call
// `run_command` with the command; the truncation is the dispatcher's default.
TEST_CASE("run_command result is smart-truncated by default",
          "[integration][llm][shell][output_filter]")
{
    auto& h = harness();

    PromptResult r = h.prompt(
        "Use the run_command tool to execute the shell command "
        "`cmd /c for /L %i in (1,1,300) do @echo line %i` and report back. "
        "I just want you to run the tool and then describe what you got.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("run_command"));

    const auto* res = r.find_result_for("run_command");
    REQUIRE(res != nullptr);
    // Default head+tail is 50 lines per side. With 300 lines total the
    // helper must elide the middle ~200 and emit the marker.
    REQUIRE(res->display.find("lines elided")     != std::string::npos);
    REQUIRE(res->display.find(".locus/locus.log") != std::string::npos);
    // First line of the tail (line 251) must be present; a line from the
    // middle (line 150) must NOT be in the trimmed body.
    REQUIRE(res->display.find("line 1\n")   != std::string::npos);  // head
    REQUIRE(res->display.find("line 300")   != std::string::npos);  // tail
    REQUIRE(res->display.find("line 150\n") == std::string::npos);  // middle
}

// run_command output filter: explicit regex mode. The agent must pass
// output_filter_mode="regex" + a pattern, and the tool result must come
// back as the "[output_filter regex: K of N lines matched]" summary form
// rather than the head_tail elision form. Verifies the schema + arg
// routing end-to-end through the LLM.
TEST_CASE("run_command honours output_filter_mode=regex",
          "[integration][llm][shell][output_filter]")
{
    auto& h = harness();

    // The command produces 30 numbered lines; only a handful contain the
    // word "ERROR" so a `regex` filter for that word must come back as a
    // small summary block.
    PromptResult r = h.prompt(
        "Call the run_command tool exactly once with these arguments: "
        "command set to "
        "`cmd /c (echo line 1 & echo ERROR foo & echo line 3 & echo line 4 & "
        "echo ERROR bar & echo line 6 & echo line 7 & echo line 8 & "
        "echo ERROR baz & echo line 10)`, and the following filter "
        "parameters: output_filter_mode set to \"regex\", "
        "output_filter_pattern set to \"ERROR\", output_filter_lines set "
        "to 10. Run it now, do not paraphrase the params -- pass them "
        "verbatim. Then tell me what came back.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("run_command"));

    const auto* call = r.find_tool_call("run_command");
    REQUIRE(call != nullptr);
    // The LLM must have routed the filter params through the call args.
    REQUIRE(call->args.contains("output_filter_mode"));
    REQUIRE(call->args["output_filter_mode"].get<std::string>() == "regex");
    REQUIRE(call->args.contains("output_filter_pattern"));

    const auto* res = r.find_result_for("run_command");
    REQUIRE(res != nullptr);
    // Filtered form leads with the summary; head_tail elision marker must
    // NOT be present (that's the default path we're bypassing).
    REQUIRE(res->display.find("output_filter regex") != std::string::npos);
    REQUIRE(res->display.find("ERROR foo")           != std::string::npos);
    REQUIRE(res->display.find("ERROR baz")           != std::string::npos);
    REQUIRE(res->display.find("lines elided")        == std::string::npos);
}

TEST_CASE("agent lists a directory", "[integration][llm][fs]")
{
    auto& h = harness();

    PromptResult r = h.prompt(
        "Use the list_directory tool to list the contents of "
        "`tests/sample_docs`. Report each file name.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.tool_called("list_directory"));

    const auto* res = r.find_result_for("list_directory");
    REQUIRE(res != nullptr);
    REQUIRE(res->display.find("sample.pdf")  != std::string::npos);
    REQUIRE(res->display.find("sample.docx") != std::string::npos);
    REQUIRE(res->display.find("sample.xlsx") != std::string::npos);
}
