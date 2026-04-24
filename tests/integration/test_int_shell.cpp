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
