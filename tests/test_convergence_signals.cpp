// S6.21 -- unit tests for the pure convergence/honesty signals + the
// edit_file self-healing remedy text. The side-effecting consumers
// (AgentTurnRunner::check_all_dropped / check_edit_ambiguity /
// check_unverified_success / maybe_inject_compaction_breadcrumb) are
// integration-shaped (need a live turn); these pin the pure decisions and the
// tool-level remedy strings the way S6.20's build_error_detector tests do.

#include "agent/convergence_signals.h"

#include "support/fake_workspace_services.h"
#include "tools/file_tools.h"
#include "tools/shared.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

using locus::AllDroppedAction;
using locus::classify_all_dropped;
using locus::is_edit_ambiguity_error;
using locus::claims_unverified_success;

// ===========================================================================
// Task 1 -- classify_all_dropped
// ===========================================================================

TEST_CASE("classify_all_dropped: a delivered call is never an all-dropped round",
          "[s6.21][all_dropped]")
{
    // delivered>=1 -> none regardless of dropped count or the nudged flag.
    REQUIRE(classify_all_dropped(1, 0, false) == AllDroppedAction::none);
    REQUIRE(classify_all_dropped(2, 1, false) == AllDroppedAction::none);
    REQUIRE(classify_all_dropped(1, 3, true)  == AllDroppedAction::none);
}

TEST_CASE("classify_all_dropped: no tool calls at all is not an all-dropped round",
          "[s6.21][all_dropped]")
{
    // delivered==0 && dropped==0 -> a plain text round, not the malformed loop.
    REQUIRE(classify_all_dropped(0, 0, false) == AllDroppedAction::none);
    REQUIRE(classify_all_dropped(0, 0, true)  == AllDroppedAction::none);
}

TEST_CASE("classify_all_dropped: first all-dropped round nudges, second aborts",
          "[s6.21][all_dropped]")
{
    // delivered==0 && dropped>=1: nudge the first time, abort after a nudge.
    REQUIRE(classify_all_dropped(0, 1, /*already_nudged=*/false)
            == AllDroppedAction::nudge);
    REQUIRE(classify_all_dropped(0, 2, /*already_nudged=*/false)
            == AllDroppedAction::nudge);
    REQUIRE(classify_all_dropped(0, 1, /*already_nudged=*/true)
            == AllDroppedAction::abort);
    REQUIRE(classify_all_dropped(0, 5, /*already_nudged=*/true)
            == AllDroppedAction::abort);
}

// ===========================================================================
// Task 2 -- is_edit_ambiguity_error
// ===========================================================================

TEST_CASE("is_edit_ambiguity_error: ambiguous-match error is recognized",
          "[s6.21][edit_file_hint]")
{
    // The exact string apply_single_edit emits (S6.21 remedy form).
    std::string out =
        "Error: 'old_string' matches 3 locations in src/main.cpp. "
        "Set replace_all=true to replace all 3, or add more surrounding context "
        "to the anchor so it matches exactly one location.";
    REQUIRE(is_edit_ambiguity_error(out));
}

TEST_CASE("is_edit_ambiguity_error: not-found error is recognized",
          "[s6.21][edit_file_hint]")
{
    std::string out =
        "Error: 'old_string' not found in src/main.cpp (exact match required, "
        "including whitespace). Re-read the file with read_file and copy the "
        "exact text including indentation; for a large change prefer write_file "
        "with overwrite=true.";
    REQUIRE(is_edit_ambiguity_error(out));
}

TEST_CASE("is_edit_ambiguity_error: a successful edit or unrelated error is NOT flagged",
          "[s6.21][edit_file_hint]")
{
    REQUIRE_FALSE(is_edit_ambiguity_error("Edited src/main.cpp"));
    REQUIRE_FALSE(is_edit_ambiguity_error(
        "Applied 2 edits to src/main.cpp"));
    REQUIRE_FALSE(is_edit_ambiguity_error(
        "Error: file does not exist: src/foo.cpp (use write_file to create "
        "a new file)"));
    // An empty-old_string error is a different failure mode -- not the
    // anchor-loop case.
    REQUIRE_FALSE(is_edit_ambiguity_error(
        "Error: 'old_string' is empty in src/main.cpp"));
    REQUIRE_FALSE(is_edit_ambiguity_error(""));
}

// ===========================================================================
// Task 2 -- EditFileTool emits the model-actionable remedy in the result body
// ===========================================================================

namespace {

fs::path make_tmp()
{
    auto root = fs::temp_directory_path() /
                ("locus_s621_" + std::to_string(::time(nullptr)));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& root, const std::string& rel,
                const std::string& body)
{
    fs::path full = root / rel;
    fs::create_directories(full.parent_path());
    std::ofstream(full, std::ios::binary) << body;
}

void prime_read(const fs::path& tmp, const std::string& rel)
{
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::ReadFileTool reader;
    locus::ToolCall call{"r", "read_file", {{"path", rel}}};
    auto r = reader.execute(call, ws);
    REQUIRE(r.success);
}

locus::ToolCall one_edit(const std::string& rel, const std::string& old_s,
                         const std::string& new_s)
{
    nlohmann::json edit = {{"old_string", old_s}, {"new_string", new_s}};
    return {"c1", "edit_file",
            {{"file_path", rel},
             {"edits_array", nlohmann::json::array({edit})}}};
}

} // namespace

TEST_CASE("EditFileTool: ambiguous match result names replace_all remedy",
          "[s6.21][edit_file_hint]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_tmp();
    write_file(tmp, "f.txt", "foo foo foo");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    auto r = tool.execute(one_edit("f.txt", "foo", "bar"), ws);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("matches 3 locations"));
    // S6.21 Task 2 -- the model-actionable remedy must be in the body.
    REQUIRE_THAT(r.content, ContainsSubstring("replace_all=true"));
    REQUIRE_THAT(r.content, ContainsSubstring("surrounding context"));
    // The result is recognized by the loop detector too.
    REQUIRE(is_edit_ambiguity_error(r.content));

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: not-found result names write_file overwrite remedy",
          "[s6.21][edit_file_hint]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_tmp();
    write_file(tmp, "f.txt", "alpha beta gamma");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    auto r = tool.execute(one_edit("f.txt", "delta", "DELTA"), ws);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("not found"));
    // S6.21 Task 2 -- both recoveries spelled out.
    REQUIRE_THAT(r.content, ContainsSubstring("read_file"));
    REQUIRE_THAT(r.content, ContainsSubstring("overwrite=true"));
    REQUIRE(is_edit_ambiguity_error(r.content));

    fs::remove_all(tmp);
}

// ===========================================================================
// Task 3 -- claims_unverified_success
// ===========================================================================

TEST_CASE("claims_unverified_success: success phrase + no verifying call -> true",
          "[s6.21][success_guard]")
{
    std::vector<std::string> none;
    REQUIRE(claims_unverified_success(
        "Great -- the build now succeeds and the app is ready.", none));
    REQUIRE(claims_unverified_success(
        "The build succeeds and produced frame0.ppm.", none));
    REQUIRE(claims_unverified_success(
        "It compiles cleanly now.", none));
    REQUIRE(claims_unverified_success(
        "The exe runs and renders the scene.", none));
}

TEST_CASE("claims_unverified_success: a verifying tool this round suppresses it",
          "[s6.21][success_guard]")
{
    // run_command this round -> the claim is grounded; stay silent.
    REQUIRE_FALSE(claims_unverified_success(
        "The build now succeeds.", {"run_command"}));
    // read_file of the artifact also counts as verification.
    REQUIRE_FALSE(claims_unverified_success(
        "frame0.ppm was created.", {"read_file"}));
    // list_directory showing the artifact counts.
    REQUIRE_FALSE(claims_unverified_success(
        "The exe runs.", {"list_directory"}));
    // search_* tools count as well.
    REQUIRE_FALSE(claims_unverified_success(
        "The build succeeds.", {"search_text"}));
}

TEST_CASE("claims_unverified_success: a non-verifying tool does NOT suppress it",
          "[s6.21][success_guard]")
{
    // The model called a tool, but not one that could confirm a build/run.
    REQUIRE(claims_unverified_success(
        "The build now succeeds.", {"add_memory"}));
}

TEST_CASE("claims_unverified_success: honest summaries without a success claim -> false",
          "[s6.21][success_guard]")
{
    std::vector<std::string> none;
    REQUIRE_FALSE(claims_unverified_success(
        "I edited the readback block; please build and let me know.", none));
    REQUIRE_FALSE(claims_unverified_success(
        "Done. I made the change you asked for.", none));
    REQUIRE_FALSE(claims_unverified_success(
        "I think this should work, but you should verify the build.", none));
    REQUIRE_FALSE(claims_unverified_success("", none));
}
