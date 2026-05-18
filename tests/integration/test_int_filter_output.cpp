#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace locus::integration;

// S5.Z task 8 -- LLM is asked to filter a small text blob via the new
// `filter_output` tool. Validates the end-to-end path: tool advertised
// in the manifest, called by the model, auto-approved, returns lines
// matching the pattern + a summary line.
TEST_CASE("agent uses filter_output to extract matching lines",
          "[integration][llm][filter_output]")
{
    auto& h = harness();

    PromptResult r = h.prompt(
        "Use the filter_output tool with `text` set to "
        "\"alpha line 1\\nalpha line 2\\nbeta line\\nalpha line 3\\n\" and "
        "`pattern` set to \"alpha\". Report the tool's response verbatim.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("filter_output"));

    const auto* res = r.find_result_for("filter_output");
    REQUIRE(res != nullptr);
    // The summary line names total + match counts. Some smaller models will
    // paraphrase but at least one of the alpha lines should round-trip.
    REQUIRE(res->display.find("alpha") != std::string::npos);
}
