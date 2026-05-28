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
    h.agent().reset_conversation();  // sibling cases share history; clear it
                                      // so QualityMonitor doesn't trip on a
                                      // similar prior tool_call.

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

// S6.18 E -- Pass-5 replay: the Pass-5 TestLocalVibe2 transcript repeatedly
// sent {"lines":"100-152"} and never recovered until the user intervened.
// Post-S6.17 the rejection text carries the canonical-key suggestion, so a
// tool-calling model should retry with offset+length on the very next round
// and the second call should succeed. This case asserts that round-trip
// end-to-end. It is the only piece of S6.17's Task D that wasn't exercised
// live during the acceptance run (the model used canonical args on all 5
// reads), so this catches regressions to the rejection text + the
// describe_tool retry surface in tandem.

TEST_CASE("read_file alias rejection: model recovers with canonical args next round",
          "[integration][llm][s6.18][read_file_alias]")
{
    auto& h = harness();
    h.agent().reset_conversation();  // sibling cases share history.

    const std::string rel_path = "tests/integration_tmp/alias_recover.txt";
    const fs::path    abs_path = h.workspace_root() / rel_path;
    fs::create_directories(abs_path.parent_path());
    {
        std::ofstream out(abs_path, std::ios::binary);
        for (int i = 1; i <= 200; ++i)
            out << "line " << i << ": filler content for alias-recover test\n";
    }

    // Pass-5 phrasing: ask for a specific line range. The model is free to
    // pick whatever arg shape it wants on the first try; we assert the
    // recovery path, not the first-call shape.
    PromptResult r = h.prompt(
        "Read lines 100 through 152 of " + rel_path + ". "
        "After you have the content, reply with a one-sentence summary of "
        "what those lines look like.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("read_file"));

    // Collect every read_file call/result pair in order. We expect at least
    // one call; if the first used the alias, we expect a second using the
    // canonical args.
    std::vector<const ObservedToolCall*> calls;
    for (const auto& c : r.tool_calls) {
        if (c.tool_name == "read_file") calls.push_back(&c);
    }
    REQUIRE_FALSE(calls.empty());

    // If the model went straight to canonical offset+length on the first
    // try, the alias-recovery path didn't fire and this run can't verify
    // it. Skip with a WARN so the suite still reports the test as run.
    bool first_used_alias = false;
    for (const char* k : {"lines", "line_range", "range"}) {
        if (calls.front()->args.contains(k)) { first_used_alias = true; break; }
    }
    if (!first_used_alias) {
        WARN("Model went straight to canonical args; alias-recovery path "
             "not exercised this run. Try a different model.");
        fs::remove(abs_path);
        return;
    }

    // (b) The rejection message for the first call must steer toward the
    // canonical shape. find_result_for picks the first matching tool_name
    // -- on this run that is the first (rejected) call.
    const auto* first_res = r.find_result_for("read_file");
    REQUIRE(first_res != nullptr);
    INFO("first read_file result display: " << first_res->display);
    REQUIRE(first_res->display.find("offset") != std::string::npos);
    REQUIRE(first_res->display.find("length") != std::string::npos);

    // (c) + (d): the model retried with canonical args AND the retry
    // succeeded. We pair each canonical call with its result by call_id and
    // check that the result body looks like file content rather than the
    // rejection text.
    bool recovered = false;
    for (size_t i = 1; i < calls.size(); ++i) {
        const auto& c = *calls[i];
        if (!c.args.contains("offset") || !c.args.contains("length")) continue;
        const ObservedToolResult* match = nullptr;
        for (const auto& tr : r.tool_results) {
            if (tr.call_id == c.id) { match = &tr; break; }
        }
        if (!match) continue;
        // A successful read_file display contains the rendered file body --
        // the alias-recover.txt fixture writes "line N:" per row, so any
        // such substring inside the asked-for window proves the retry came
        // back with content. The rejection text never contains "line 1" /
        // "line 2" etc.
        if (match->display.find("unrecognized argument") != std::string::npos)
            continue;
        if (match->display.find("line 100:") != std::string::npos
            || match->display.find("line 101:") != std::string::npos
            || match->display.find("line 150:") != std::string::npos) {
            recovered = true;
            break;
        }
    }
    INFO("read_file calls in this turn: " << calls.size());
    REQUIRE(recovered);

    fs::remove(abs_path);
}
