// S6.17 Task D -- read_file alias rejection (live LLM).
//
// Pre-S6.17 baseline: read_file with `lines:"100-200"` silently fell through
// to the default `offset=1, length=100` because `value(key, default)` swallows
// unknown keys. The model couldn't see anything was wrong and retried the
// same shape 6 times in TestLocalVibe2 Pass 2/5 before giving up.
//
// Post-S6.17: `tools::reject_unknown_keys` catches the unknown `lines` key
// before ReadFileTool::execute reads any args. The error message names
// `lines` explicitly AND points at the canonical `offset` + `length` shape
// via the alias map in src/tools/shared.cpp.
//
// This test instructs the model to send the `lines:` shape verbatim and
// asserts the tool result content carries the rejection + suggestion. It
// does NOT assert the model recovers on a follow-up turn -- that is a
// model-quality property; the deterministic contract we care about is the
// shape of the error.

#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace locus::integration;
namespace fs = std::filesystem;

TEST_CASE("read_file rejects `lines` alias with explicit offset+length suggestion",
          "[integration][llm][s6.17][read_file_alias]")
{
    auto& h = harness();

    // Stage a multi-page file so the prompt's "read lines 100-200" framing
    // is plausible (the model often refuses obviously-broken instructions
    // when the file is too short to matter).
    const std::string rel_path = "tests/integration_tmp/alias_target.txt";
    const fs::path    abs_path = h.workspace_root() / rel_path;
    fs::create_directories(abs_path.parent_path());
    {
        std::ofstream out(abs_path, std::ios::binary);
        for (int i = 1; i <= 250; ++i)
            out << "line " << i << ": filler content for alias-rejection test\n";
    }

    // Instruct the model to send the shape we want to test. Naming the
    // exact key + value defeats the model's natural tendency to "correct"
    // to offset+length before shipping the call. We also tell it to quote
    // the tool result so the failure mode is observable in the chat output
    // if the assertion below fires (though we assert against the tool
    // result display directly, not the chat text).
    PromptResult r = h.prompt(
        "Call read_file with the argument shape: "
        "{\"path\":\"" + rel_path + "\", \"lines\":\"100-200\"}. "
        "Use the EXACT key `lines` (no offset, no length). The tool will "
        "return an error -- that is the expected behaviour. Quote the error "
        "verbatim in your reply.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("read_file"));

    const auto* call = r.find_tool_call("read_file");
    REQUIRE(call != nullptr);
    INFO("read_file call args: " << call->args.dump());
    // Confirm the model actually sent the alias -- the test below is only
    // meaningful when the LLM cooperated with the prompt. If the model
    // auto-corrected to offset+length, skip the rejection assertion (this
    // is a model-cooperation property, not a regression of our reject logic).
    if (!call->args.contains("lines")) {
        WARN("Model auto-corrected to canonical offset+length; alias path "
             "not exercised this run. Try a different model or retry.");
        return;
    }

    const auto* res = r.find_result_for("read_file");
    REQUIRE(res != nullptr);
    INFO("read_file result display: " << res->display);

    // The new error message MUST name the rejected key + steer toward the
    // canonical args. Both halves matter: the key name proves we caught
    // the right alias; the suggestion proves the helpful-recovery codepath
    // fired (vs. a generic "unknown key" without remediation).
    REQUIRE(res->display.find("unrecognized argument") != std::string::npos);
    REQUIRE(res->display.find("'lines'") != std::string::npos);
    REQUIRE(res->display.find("offset") != std::string::npos);
    REQUIRE(res->display.find("length") != std::string::npos);

    // Cleanup.
    fs::remove(abs_path);
}
