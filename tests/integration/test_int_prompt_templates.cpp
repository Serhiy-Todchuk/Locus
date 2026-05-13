// S4.X -- integration coverage for prompt templates.
//
// Drops a tiny template into the harness's per-test prompts dir, calls
// /reload (built-in slash, no LLM round), then invokes /<template_name>
// and checks the live LLM saw the expanded body and responded.

#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>

using namespace locus::integration;
using Catch::Matchers::ContainsSubstring;

namespace fs = std::filesystem;

namespace {

void write_template(const fs::path& dir,
                    const std::string& name,
                    const std::string& body)
{
    fs::create_directories(dir);
    std::ofstream out(dir / (name + ".md"), std::ios::binary | std::ios::trunc);
    out << body;
}

} // namespace

TEST_CASE("/reload picks up a fresh template; /<name> expands into the LLM input",
          "[integration][llm][slash][s4.x]")
{
    auto& h = harness();
    auto prompts = h.tmp_dir() / "prompts";

    // Body deliberately contains a high-cardinality token that's vanishingly
    // unlikely to appear by chance in the LLM's output. The {0} placeholder
    // is filled by the slash argument.
    write_template(prompts, "s4x_echo_marker",
                   "Repeat exactly this token in your reply, with no other "
                   "words: ZZ-MARKER-S4X-{0}-ENDZZ");

    // The agent thread was constructed before this file existed, so a fresh
    // /reload is required to register it. /reload is a built-in slash and
    // does not call the LLM.
    {
        PromptResult r = h.prompt("/reload");
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.errors.empty());
        REQUIRE_THAT(r.tokens, ContainsSubstring("Reloaded prompt templates"));
    }

    // /<name> expands and lands as a user message; the LLM should run a
    // single round and emit the marker. Small models occasionally wrap the
    // token in punctuation, so we look for the stable substring only.
    {
        PromptResult r = h.prompt("/s4x_echo_marker 7");
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.errors.empty());
        REQUIRE_THAT(r.tokens, ContainsSubstring("ZZ-MARKER-S4X-7-ENDZZ"));
    }

    std::error_code ec;
    fs::remove(prompts / "s4x_echo_marker.md", ec);
}

TEST_CASE("Unknown /<name> with no matching template surfaces unknown-command",
          "[integration][slash][s4.x]")
{
    auto& h = harness();

    PromptResult r = h.prompt("/definitely_not_a_real_command_or_template");
    REQUIRE_FALSE(r.timed_out);
    // Unknown command is delivered on the error channel; the LLM is not
    // called, so there should be no tool activity and no streamed tokens.
    REQUIRE_FALSE(r.errors.empty());

    bool found = false;
    for (auto& e : r.errors) {
        if (e.find("Unknown command") != std::string::npos) { found = true; break; }
    }
    REQUIRE(found);
}
