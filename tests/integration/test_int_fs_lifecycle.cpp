#include "harness_fixture.h"

#include "index_query.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

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

TEST_CASE("full file lifecycle via agent + tools", "[integration][llm][fs]")
{
    auto& h = harness();

    const fs::path rel_path_fs  = fs::path("tests") / "integration_tmp" / "notes.md";
    const std::string rel_path  = "tests/integration_tmp/notes.md";
    const fs::path abs_path     = h.workspace_root() / rel_path_fs;

    // NOTE: each Catch2 SECTION re-runs the TEST_CASE body, so file state
    // persists across sections only via disk. We deliberately do NOT wipe the
    // file here; sections are ordered: create → edit → index → delete.

    SECTION("create via write_file") {
        PromptResult r = h.prompt(
            "Create a new file at `" + rel_path + "` using the write_file "
            "tool. Its content must be three short paragraphs explaining why "
            "parsers based on context-free grammars are useful for language "
            "tooling. You MUST use the exact spelling `tree-sitter` (all "
            "lowercase, hyphenated) everywhere you refer to tree-sitter in "
            "the text — do not capitalise it, do not write it as one word.");

        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.errors.empty());
        REQUIRE(r.tool_called("write_file"));
        REQUIRE(fs::exists(abs_path));

        std::string content = read_whole(abs_path);
        REQUIRE(content.size() > 50);
        REQUIRE(content.find("tree-sitter") != std::string::npos);
    }

    SECTION("edit via edit_file with replace_all") {
        // Precondition — the file from the create section must be on disk.
        // Write a known-content file if the previous section didn't run first.
        if (!fs::exists(abs_path)) {
            fs::create_directories(abs_path.parent_path());
            std::ofstream(abs_path) << "tree-sitter is a parser. tree-sitter is fast.\n";
        }

        PromptResult r = h.prompt(
            "The file `" + rel_path + "` contains the word `tree-sitter` "
            "(lowercase, hyphenated) in multiple places. First read the "
            "file, then use edit_file with replace_all=true to change every "
            "occurrence of the exact lowercase string `tree-sitter` to the "
            "uppercase string `TREE-SITTER`. Keep the rest of the file "
            "untouched.");

        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.errors.empty());
        // ReadTracker forces read_file before edit_file on the same path.
        REQUIRE(r.tool_called("read_file"));
        REQUIRE(r.tool_called("edit_file"));

        std::string content = read_whole(abs_path);
        REQUIRE(content.find("TREE-SITTER") != std::string::npos);
    }

    SECTION("index picks up the new file") {
        // Ensure the indexer has caught up with the watcher events emitted by
        // the prior write/edit tool calls before asserting.
        h.wait_for_index_idle();

        // Deterministic check: ask IndexQuery directly whether notes.md is
        // present in the index. LLM-driven FTS queries over arbitrary
        // tool-written content are flaky (tokenizer quirks + non-deterministic
        // LLM output); coverage of the LLM search path already lives in
        // test_int_search.cpp against stable repo content.
        auto files = h.workspace().query().list_directory(
            "tests/integration_tmp", 0);
        bool indexed = std::any_of(files.begin(), files.end(),
            [](const locus::FileEntry& f) {
                return f.path.find("notes.md") != std::string::npos;
            });
        REQUIRE(indexed);
    }

    SECTION("delete via delete_file") {
        if (!fs::exists(abs_path)) {
            fs::create_directories(abs_path.parent_path());
            std::ofstream(abs_path) << "stub\n";
        }
        PromptResult r = h.prompt(
            "Delete the file `" + rel_path + "` using the delete_file tool.");

        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.errors.empty());
        REQUIRE(r.tool_called("delete_file"));
        REQUIRE_FALSE(fs::exists(abs_path));
    }
}
