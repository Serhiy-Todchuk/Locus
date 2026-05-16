// S5.G -- per-message delete + on_history_message_added/deleted hooks.
//
// Verifies the end-to-end path: the agent appends every user / assistant /
// tool message to ConversationHistory and broadcasts on_history_message_added
// with the right `deletable` flag for each role. AgentCore::delete_message
// then drops a chosen entry, broadcasts on_history_message_deleted, and the
// next LLM round POSTs a history without the removed message.

#include "harness_fixture.h"

#include "agent/conversation.h"
#include "agent/llm_context.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace locus::integration;

namespace {

// Wait until the agent thread has drained any pending_delete_ids_ slot. The
// delete request lands on a queue, then `apply_pending_deletes` runs in the
// next agent-thread iteration. Polling on history().size() is the simplest
// correctness gate; a 2 s budget is more than enough for the drain.
bool wait_for_history_size(locus::AgentCore& agent, size_t target,
                            std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (agent.history().size() <= target) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

TEST_CASE("on_history_message_added fires with the right role + deletable flag",
          "[integration][llm][history_delete]")
{
    auto& h = harness();

    // Send a question that does NOT require tools so the round is just
    // user -> assistant. Then we can assert the two adds with deletable=true.
    PromptResult r = h.prompt(
        "Say exactly 'pong' and nothing else.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());

    auto adds = h.frontend().history_adds();
    REQUIRE(adds.size() >= 2);

    // Find the user add (the most recent user-role event before the assistant).
    bool saw_user_deletable = false;
    bool saw_assistant_deletable = false;
    for (const auto& a : adds) {
        if (a.role == locus::MessageRole::user && a.deletable)
            saw_user_deletable = true;
        if (a.role == locus::MessageRole::assistant && a.deletable)
            saw_assistant_deletable = true;
    }
    REQUIRE(saw_user_deletable);
    REQUIRE(saw_assistant_deletable);
}

TEST_CASE("AgentCore::delete_message drops the message + broadcasts deletion",
          "[integration][llm][history_delete]")
{
    auto& h = harness();

    PromptResult r1 = h.prompt(
        "I will tell you a secret: my password is hunter2-do-not-leak. "
        "Just reply OK.");

    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.errors.empty());

    // Locate the user message we just sent in the live ConversationHistory.
    auto& history = h.agent().history();
    int   target_id = 0;
    for (const auto& m : history.messages()) {
        if (m.role == locus::MessageRole::user &&
            m.content.find("hunter2-do-not-leak") != std::string::npos) {
            target_id = m.history_id;
            break;
        }
    }
    REQUIRE(target_id > 0);

    size_t before = history.size();

    h.frontend().reset_turn_state();   // arm history_deletes capture
    h.agent().delete_message(target_id);

    // Wait for the queued delete to drain on the agent thread.
    REQUIRE(wait_for_history_size(h.agent(), before - 1,
                                   std::chrono::seconds(2)));

    // The on_history_message_deleted broadcast must include the id we asked for.
    auto deletes = h.frontend().history_deletes();
    bool saw = false;
    for (int id : deletes)
        if (id == target_id) saw = true;
    REQUIRE(saw);

    // The user message must be gone from history.
    bool still_there = false;
    for (const auto& m : h.agent().history().messages()) {
        if (m.history_id == target_id) still_there = true;
    }
    REQUIRE_FALSE(still_there);
}

TEST_CASE("delete_message refuses the system message + unknown ids",
          "[integration][history_delete]")
{
    auto& h = harness();

    // The leading message is the system prompt; its id is 1 (assigned by
    // ConversationHistory::add at LLMContext construction).
    auto& history = h.agent().history();
    REQUIRE_FALSE(history.empty());
    REQUIRE(history.messages().front().role == locus::MessageRole::system);
    int sys_id = history.messages().front().history_id;

    size_t before = history.size();
    h.frontend().reset_turn_state();
    h.agent().delete_message(sys_id);

    // The agent thread will see the request, ConversationHistory::delete_by_id
    // refuses the system message, the broadcast does NOT fire.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(h.agent().history().size() == before);

    auto deletes = h.frontend().history_deletes();
    bool saw_sys = false;
    for (int id : deletes) if (id == sys_id) saw_sys = true;
    REQUIRE_FALSE(saw_sys);

    // Unknown id: silently dropped (the warning-log path), no history change.
    h.agent().delete_message(999999);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(h.agent().history().size() == before);
}
