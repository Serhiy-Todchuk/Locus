// S4.B -- checkpoint & undo.
//
// Verifies the end-to-end path: a mutating tool call (write_file / edit_file
// / delete_file) snapshots the prior file state via CheckpointStore; the
// `/undo` slash command then restores that state.
//
// We assert against the file system (file present / absent / content matches)
// and the agent's emitted text -- not on LLM prose, since `/undo` is handled
// by AgentCore directly without an LLM round-trip.

#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace locus::integration;
namespace fs = std::filesystem;

namespace {

void write_file_text(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

std::string read_whole(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("/undo restores a file the agent wrote", "[integration][llm][undo]")
{
    auto& h = harness();

    const std::string rel = "tests/integration_tmp/s4b_undo_create.txt";
    const fs::path    abs = h.workspace_root() / rel;

    std::error_code ec;
    fs::remove(abs, ec);
    REQUIRE_FALSE(fs::exists(abs));

    // Turn 1: agent creates the file. The dispatcher's maybe_snapshot path
    // takes a checkpoint that records "this path didn't exist before".
    PromptResult r1 = h.prompt(
        "Use the write_file tool to create a new file at `" + rel + "` whose "
        "content is exactly the single line: created by agent. "
        "After the tool returns, reply with exactly the single word DONE.");

    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.errors.empty());
    REQUIRE(r1.tool_called("write_file"));
    REQUIRE(fs::exists(abs));

    // Turn 2: /undo runs synchronously through AgentCore::try_slash_command,
    // emits its summary via on_token, and bypasses the LLM entirely.
    PromptResult r2 = h.prompt("/undo");
    REQUIRE_FALSE(r2.timed_out);
    REQUIRE(r2.errors.empty());
    // /undo doesn't dispatch tools.
    REQUIRE(r2.tool_calls.empty());

    // The summary text reaches the harness as on_token output.
    INFO("undo output: " << r2.tokens);
    REQUIRE(r2.tokens.find("Undo turn") != std::string::npos);

    // The file existed before turn 1 = no => after undo it must be gone.
    REQUIRE_FALSE(fs::exists(abs));
}

TEST_CASE("/undo restores file contents after an edit", "[integration][llm][undo]")
{
    auto& h = harness();

    const std::string rel = "tests/integration_tmp/s4b_undo_edit.md";
    const fs::path    abs = h.workspace_root() / rel;

    // Seed the file with known content. Use plain ASCII so the LLM's
    // edit_file `old_string` argument matches without escaping surprises.
    const std::string original  = "tree-sitter is fast.\n";
    const std::string mutated   = "TREE-SITTER is fast.\n";
    write_file_text(abs, original);

    // Turn 1: agent reads + edits the file. ReadTracker forces read_file
    // before edit_file; the dispatcher snapshots the original bytes before
    // the rename lands.
    PromptResult r1 = h.prompt(
        "First read the file `" + rel + "` with read_file. Then use edit_file "
        "with replace_all=true to change every occurrence of the exact "
        "lowercase string `tree-sitter` to the uppercase string "
        "`TREE-SITTER`. After the tool returns, reply with exactly DONE.");

    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.errors.empty());
    REQUIRE(r1.tool_called("edit_file"));

    // Confirm the edit landed (sanity check; the file should have new bytes).
    REQUIRE(read_whole(abs) == mutated);

    // Turn 2: /undo restores the snapshot taken before the edit.
    PromptResult r2 = h.prompt("/undo");
    REQUIRE_FALSE(r2.timed_out);
    REQUIRE(r2.errors.empty());
    INFO("undo output: " << r2.tokens);
    REQUIRE(r2.tokens.find("Undo turn") != std::string::npos);

    REQUIRE(fs::exists(abs));
    REQUIRE(read_whole(abs) == original);

    // Cleanup so a re-run sees the same starting state.
    std::error_code ec;
    fs::remove(abs, ec);
}

TEST_CASE("/undo with no checkpointed turns reports no-op",
          "[integration][undo]")
{
    auto& h = harness();

    // Walk every existing turn directory off the table first by undoing
    // until the dispatcher tells us nothing is left. This makes the test
    // robust to whatever earlier scenarios already ran.
    for (int i = 0; i < 50; ++i) {
        PromptResult r = h.prompt("/undo");
        REQUIRE_FALSE(r.timed_out);
        if (r.tokens.find("Nothing to undo") != std::string::npos)
            break;
        // Otherwise the harness undid one more turn -- loop again.
    }

    // One final /undo confirms the empty-state message.
    PromptResult r = h.prompt("/undo");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_calls.empty());
    INFO("undo output: " << r.tokens);
    REQUIRE(r.tokens.find("Nothing to undo") != std::string::npos);
}
