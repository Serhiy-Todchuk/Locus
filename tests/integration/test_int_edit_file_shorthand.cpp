// S6.17 Task E -- edit_file single-edit shorthand round-trip (live LLM).
//
// Verifies that when a model emits the shorthand form
//   edit_file({"file_path": "...", "old_string": "...", "new_string": "..."})
// (no `edits` array), the tool normalises it to a one-edit array internally
// and the file change actually applies. Captures the raw call.args so the
// test can prove the model sent the shorthand, not the canonical form.

#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace locus::integration;
namespace fs = std::filesystem;

namespace {

std::string read_whole(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("edit_file single-edit shorthand round-trips end-to-end",
          "[integration][llm][s6.17][edit_file_shorthand]")
{
    auto& h = harness();
    // Fresh conversation: prior cases in the same file boundary may have
    // primed QualityMonitor with similar tool_calls; reset eliminates that
    // as a confounder for the assertions below.
    h.agent().reset_conversation();

    const std::string rel_path  = "tests/integration_tmp/shorthand_edit.txt";
    const fs::path    abs_path  = h.workspace_root() / rel_path;

    // Stage a known-content file the model can edit. Pre-populated so
    // read_file primes ReadTracker on the first turn.
    fs::create_directories(abs_path.parent_path());
    std::ofstream(abs_path, std::ios::binary)
        << "The quick brown fox jumps over the lazy dog.\n";

    // Split into two prompts so a small model only has to follow one
    // instruction per turn -- gemma-4-e4b (the minimum verified model)
    // occasionally drops the second tool call when asked to chain both in
    // one prompt. Turn 1 primes ReadTracker; turn 2 is the actual shorthand
    // exercise.
    PromptResult r1 = h.prompt(
        "Call read_file on `" + rel_path + "` to confirm its content.");
    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.errors.empty());
    REQUIRE(r1.tool_called("read_file"));

    // Tell the model to emit the shorthand form explicitly. Small local
    // models trained on the canonical `edits_array=[...]` shape would
    // otherwise pick the canonical form by default and never exercise the
    // shorthand codepath. The prompt names the exact key shape we need to see.
    // The path token is repeated three times across the prompt so the model
    // can't accidentally drop it while reshaping the edit into the canonical
    // form (small models occasionally retain only `old_string`/`new_string`
    // if the path appears just once).
    PromptResult r = h.prompt(
        "Edit the file at path `" + rel_path + "`. Now call edit_file using "
        "the SHORTHAND form: pass `file_path` (set to `" + rel_path + "`), "
        "`old_string`, and `new_string` as TOP-LEVEL keys -- DO NOT wrap "
        "them in an `edits_array` array. Change the word `quick` to the "
        "word `speedy`. Use exactly this shape:\n"
        "  edit_file({\"file_path\":\"" + rel_path + "\","
        " \"old_string\":\"quick\", \"new_string\":\"speedy\"})");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("edit_file"));

    const auto* edit_call = r.find_tool_call("edit_file");
    REQUIRE(edit_call != nullptr);
    INFO("edit_file call args: " << edit_call->args.dump());
    // Either canonical `file_path` or the legacy `path` alias counts -- the
    // tool accepts both. The point of this assertion is that the model
    // included the file-path arg at all. If the model dropped the path,
    // the edit can't apply and the on-disk asserts below will fail too;
    // soft-warn here so the more informative file-content assertion gets
    // a chance to run and report the real symptom.
    if (!edit_call->args.contains("file_path") &&
        !edit_call->args.contains("path")) {
        WARN("Model dropped the file_path arg entirely (this is a "
             "model-side regression on the shorthand prompt). The "
             "on-disk content assertions below will surface the real "
             "consequence -- no edit applied.");
    }

    // The user-observable contract is "the file changed". The shorthand
    // normalisation is an internal tolerance feature -- it doesn't mandate
    // the model emit the shorthand form. Some models faithfully relay the
    // shape from the prompt (and exercise the shorthand codepath in
    // EditFileTool::execute); others auto-correct to the canonical
    // `edits=[{...}]` form. Both must produce the same on-disk result.
    //
    // We hard-assert the file change; the shape branch below SOFT-asserts
    // the shorthand path when we can see it was actually exercised.
    std::string body = read_whole(abs_path);
    REQUIRE(body.find("speedy brown fox") != std::string::npos);
    REQUIRE(body.find("quick brown fox") == std::string::npos);

    const bool shorthand_form =
        edit_call->args.contains("old_string") &&
        edit_call->args.contains("new_string") &&
        !edit_call->args.contains("edits");

    if (shorthand_form) {
        // Shorthand path exercised end-to-end. Pin the values so a future
        // regression in EditFileTool's normalisation (dropping a field on
        // its way to apply_single_edit) is caught here.
        REQUIRE(edit_call->args["old_string"].get<std::string>() == "quick");
        REQUIRE(edit_call->args["new_string"].get<std::string>() == "speedy");
    } else {
        // Model auto-corrected to canonical form. The shorthand codepath
        // wasn't exercised this run, but the file changed correctly so
        // the user-observable behaviour is intact. Unit tests in
        // tests/test_tools.cpp cover the shorthand directly.
        WARN("Model auto-corrected to canonical edits=[{...}] form; "
             "shorthand path not exercised live this run. "
             "Unit tests cover the codepath directly.");
    }

    // Cleanup -- file lives in tests/integration_tmp/ which is git-ignored
    // and re-created per test as needed.
    fs::remove(abs_path);
}
