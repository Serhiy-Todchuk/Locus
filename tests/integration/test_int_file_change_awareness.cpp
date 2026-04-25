// S4.T -- file-change awareness between turns.
//
// Verifies the end-to-end path: an external file mutation between turns
// surfaces to the LLM as a `[Files changed since last turn: ...]` prefix on
// the *next* user message in conversation history; the agent's own writes
// don't echo back into that prefix on the turn after they happen.
//
// We assert against `agent().history()` directly rather than the LLM's prose
// -- the prefix injection is deterministic; the LLM's reaction to it isn't.

#include "harness_fixture.h"

#include "agent/conversation.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

using namespace locus::integration;
namespace fs = std::filesystem;

namespace {

const locus::ChatMessage* last_user_message(const locus::ConversationHistory& h)
{
    const auto& msgs = h.messages();
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (it->role == locus::MessageRole::user) return &*it;
    }
    return nullptr;
}

void write_file_text(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

} // namespace

TEST_CASE("external edits are surfaced to the LLM as a prefix on the next user turn",
          "[integration][llm][file_change_awareness]")
{
    auto& h = harness();

    const std::string rel = "tests/integration_tmp/s4t_external.txt";
    const fs::path    abs = h.workspace_root() / rel;

    // Make sure the file isn't already on disk from an aborted prior run --
    // start clean so the diff we trigger below is unambiguous.
    std::error_code ec;
    fs::remove(abs, ec);

    // Turn 1: any prompt to bump turn_id_ and let the tracker rebase. The
    // tracker's snapshot at end-of-turn becomes the baseline for turn 2.
    PromptResult r1 = h.prompt(
        "Reply with exactly the single word OK and call no tools.");
    REQUIRE_FALSE(r1.timed_out);

    // Externally write a brand-new file (not via any tool) -- this is the
    // user-edits-between-turns case the stage was built for.
    write_file_text(abs, "hello from outside the agent\n");
    h.wait_for_index_idle();

    // Turn 2: the tracker should diff against turn-1's snapshot, see the new
    // file, and prepend the [Files changed since last turn: ...] note to the
    // user message before it lands in history.
    PromptResult r2 = h.prompt(
        "Reply with exactly the single word DONE and call no tools.");
    REQUIRE_FALSE(r2.timed_out);

    const auto* user_msg = last_user_message(h.agent().history());
    REQUIRE(user_msg != nullptr);
    REQUIRE(user_msg->content.find("[Files changed since last turn:")
            != std::string::npos);
    REQUIRE(user_msg->content.find("s4t_external.txt") != std::string::npos);

    // Cleanup so a re-run sees the same starting state.
    fs::remove(abs, ec);
}

TEST_CASE("agent's own writes don't echo back as external next turn",
          "[integration][llm][file_change_awareness]")
{
    auto& h = harness();

    const std::string rel = "tests/integration_tmp/s4t_agent_written.md";
    const fs::path    abs = h.workspace_root() / rel;

    std::error_code ec;
    fs::remove(abs, ec);

    // Turn 1: agent creates the file via write_file. The dispatcher's
    // maybe_snapshot path marks the rel path as agent-touched on the way out.
    PromptResult r1 = h.prompt(
        "Use the write_file tool to create a new file at `" + rel + "` whose "
        "content is the single line: agent wrote this. After the tool returns, "
        "reply with exactly the single word DONE.");
    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.errors.empty());
    REQUIRE(r1.tool_called("write_file"));
    REQUIRE(fs::exists(abs));

    h.wait_for_index_idle();

    // Turn 2: the agent's own write must NOT show up in next turn's
    // [Files changed since last turn: ...] prefix. The prefix may be absent
    // entirely (no other files moved); if it is present, it must not name our
    // file.
    PromptResult r2 = h.prompt(
        "Reply with exactly the single word OK and call no tools.");
    REQUIRE_FALSE(r2.timed_out);

    const auto* user_msg = last_user_message(h.agent().history());
    REQUIRE(user_msg != nullptr);

    // Either no prefix at all, or a prefix that doesn't mention our file.
    auto pos = user_msg->content.find("[Files changed since last turn:");
    if (pos != std::string::npos) {
        auto prefix_end = user_msg->content.find(']', pos);
        REQUIRE(prefix_end != std::string::npos);
        std::string prefix = user_msg->content.substr(pos, prefix_end - pos);
        INFO("prefix block: " << prefix);
        REQUIRE(prefix.find("s4t_agent_written.md") == std::string::npos);
    }

    fs::remove(abs, ec);
}
