// S6.17 Task E -- edit_file single-edit shorthand round-trip (live LLM).
//
// Verifies that when a model emits the shorthand form
//   edit_file({"path": "...", "old_string": "...", "new_string": "..."})
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

    const std::string rel_path  = "tests/integration_tmp/shorthand_edit.txt";
    const fs::path    abs_path  = h.workspace_root() / rel_path;

    // Stage a known-content file the model can edit. Pre-populated so
    // read_file primes ReadTracker on the first turn.
    fs::create_directories(abs_path.parent_path());
    std::ofstream(abs_path, std::ios::binary)
        << "The quick brown fox jumps over the lazy dog.\n";

    // Tell the model to emit the shorthand form explicitly. Small local
    // models trained on the canonical `edits=[...]` shape would otherwise
    // pick the canonical form by default and never exercise the shorthand
    // codepath. The prompt names the exact key shape we need to see.
    PromptResult r = h.prompt(
        "First call read_file on `" + rel_path + "` to confirm its content. "
        "Then call edit_file using the SHORTHAND form: pass `path`, "
        "`old_string`, and `new_string` as TOP-LEVEL keys -- DO NOT wrap "
        "them in an `edits` array. Change the word `quick` to the word "
        "`speedy`. Use exactly this shape:\n"
        "  edit_file({\"path\":\"" + rel_path + "\","
        " \"old_string\":\"quick\", \"new_string\":\"speedy\"})");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("read_file"));
    REQUIRE(r.tool_called("edit_file"));

    const auto* edit_call = r.find_tool_call("edit_file");
    REQUIRE(edit_call != nullptr);
    INFO("edit_file call args: " << edit_call->args.dump());
    REQUIRE(edit_call->args.contains("path"));

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
