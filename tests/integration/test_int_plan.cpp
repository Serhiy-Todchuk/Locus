// S4.D -- plan-mode integration tests.
//
// These exercise the full plan/execute flow against a real local LLM:
//   1. set_mode(plan) -- the agent is now restricted to propose_plan.
//   2. user prompt -- the model calls propose_plan, on_plan_proposed fires.
//   3. approve_plan -- mode flips to execute, the plan recap message is
//      auto-enqueued as a synthetic user turn, and the agent resumes.
//   4. during execute -- the model uses real tools and calls mark_step_done
//      between steps; on_plan_step_advanced fires per step, on_plan_completed
//      fires when every step is done/failed.
//
// Local 7-9B-class models (the integration baseline) are not always cooperative
// in plan mode -- they may produce a `propose_plan` with too few or trivial
// steps, or in execute mode they may forget to call `mark_step_done` between
// real tool calls. The assertions below stay deliberately loose: they verify
// the plumbing fires (events emitted, state transitions), not the quality of
// the plan content. Specific quality/coverage checks belong with a stronger
// model and live elsewhere.

#include "harness_fixture.h"

#include "agent/agent_core.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

using namespace locus;
using namespace locus::integration;
using namespace std::chrono_literals;

namespace {

// Wait until the agent reports the requested mode, polling at 50ms. Returns
// false on timeout. Mode changes go through the agent's queue (apply_pending_
// mode_change drains them between turns), so set_mode() returns instantly but
// the actual transition takes one queue tick.
bool wait_for_mode(AgentCore& core, AgentMode want,
                   std::chrono::milliseconds timeout = 3s)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (core.mode() == want) return true;
        std::this_thread::sleep_for(50ms);
    }
    return core.mode() == want;
}

} // namespace

// ===========================================================================
// Plumbing test: set_mode(plan) actually filters the tool catalog
// ===========================================================================

TEST_CASE("plan mode restricts the API tool catalog to propose_plan only",
          "[integration][llm][plan]")
{
    auto& h = harness();

    // Drop history clean before fiddling with mode -- prior tests in this
    // harness may have left a long conversation history that makes prompt
    // tokens balloon and the LLM stall on long prefill.
    h.agent().reset_conversation();

    h.agent().set_mode(AgentMode::plan);
    REQUIRE(wait_for_mode(h.agent(), AgentMode::plan));

    PromptResult r = h.prompt(
        "Outline a tiny three-step plan for adding logging to a small C++ "
        "program. Use the propose_plan tool. Keep the steps short -- one "
        "sentence each. Do not call any other tool.",
        std::chrono::minutes(3));

    INFO("tokens: " << r.tokens);
    REQUIRE_FALSE(r.timed_out);

    // Either the model called propose_plan (good) OR it called something
    // else (which means the catalog filter is broken -- fail loudly). It
    // must NOT have called read_file / write_file / search etc.
    bool saw_propose = false;
    for (auto& c : r.tool_calls) {
        INFO("tool seen: " << c.tool_name);
        if (c.tool_name == "propose_plan") saw_propose = true;
        else REQUIRE(c.tool_name == "propose_plan");  // would have failed above
    }
    // Some tiny models refuse to call any tool when their catalog has only
    // one option. We accept zero calls but require that NO non-plan tool
    // ever fires (the assertion above is the actual gate).
    if (!saw_propose) {
        WARN("Model did not call propose_plan; check on_plan_proposed below "
             "for whether the agent at least observed plan-mode behavior.");
    }

    // Reset back to chat for subsequent tests.
    h.agent().set_mode(AgentMode::chat);
    REQUIRE(wait_for_mode(h.agent(), AgentMode::chat));
}

// ===========================================================================
// Full happy path: propose -> approve -> execute -> mark_step_done -> done
// ===========================================================================

TEST_CASE("propose_plan + approve + at least one mark_step_done fires",
          "[integration][llm][plan]")
{
    auto& h = harness();
    h.agent().reset_conversation();
    h.agent().set_mode(AgentMode::plan);
    REQUIRE(wait_for_mode(h.agent(), AgentMode::plan));

    // First turn: ask for a tiny, safe plan that the model can actually
    // execute (read_file + reply). Steps are deliberately small so a 7-9B
    // model has a chance to call mark_step_done between them.
    PromptResult r1 = h.prompt(
        "Use propose_plan to outline a TWO-step plan: "
        "step 1 = read CLAUDE.md (tools_needed: [\"read_file\"]); "
        "step 2 = report one sentence about what Locus is "
        "(tools_needed: []). Keep step descriptions one sentence each.",
        std::chrono::minutes(3));
    INFO("turn 1 tokens: " << r1.tokens);
    REQUIRE_FALSE(r1.timed_out);

    auto plan = h.frontend().last_plan_proposed();
    if (!plan.has_value()) {
        WARN("Model did not call propose_plan; skipping the rest of the case "
             "so the run isn't a flake. Verified at unit level in "
             "test_plan_mode.cpp.");
        h.agent().set_mode(AgentMode::chat);
        return;
    }
    CHECK(plan->steps.size() >= 1);
    INFO("plan id=" << plan->id << " steps=" << plan->steps.size());

    // Approve. AgentCore enqueues a recap message, mode flips to execute,
    // a new turn fires automatically. arm_next_turn() resets the
    // turn-complete latch so wait_for_turn blocks for the auto-resume.
    h.frontend().arm_next_turn();
    h.agent().approve_plan();

    // Wait long enough for the resumed turn to do its thing. Local models
    // can take minutes on a fresh prefill -- 4 minutes covers gemma-4-e4b
    // on a consumer GPU.
    REQUIRE(h.frontend().wait_for_turn(std::chrono::minutes(4)));

    // Mode advanced to execute (at minimum -- the model may continue and
    // eventually flip back to chat, but execute must be in the trail).
    auto modes = h.frontend().mode_changes();
    bool saw_execute = std::any_of(modes.begin(), modes.end(),
        [](AgentMode m) { return m == AgentMode::execute; });
    REQUIRE(saw_execute);

    // The agent SHOULD have called at least one tool while executing
    // (mark_step_done is the explicit one we want; any of the catalog
    // tools that were actually used count as proof that execute mode
    // unlocked the catalog). Quality of the execute trace is model-side
    // variance; we just want evidence the mode switched.
    auto all_tool_calls = h.frontend().tool_calls();
    int total_tools = static_cast<int>(all_tool_calls.size());
    INFO("execute-phase tool calls: " << total_tools);
    for (auto& c : all_tool_calls) INFO("  " << c.tool_name);

    // The on_plan_step_advanced callback fires only when mark_step_done is
    // called successfully. Local models often skip it; treat zero advances
    // as a soft warning, not a hard failure.
    auto step_advances = h.frontend().plan_step_advances();
    if (step_advances.empty()) {
        WARN("Model never called mark_step_done. Plumbing verified up to "
             "approve+mode_changed; per-step advance is model-side variance.");
    } else {
        // If at least one step advanced, the recorded plan_id must match
        // the proposed plan's id.
        CHECK(step_advances[0].plan_id == plan->id);
    }

    // Cleanup: back to chat so subsequent tests start fresh.
    h.agent().set_mode(AgentMode::chat);
    (void)wait_for_mode(h.agent(), AgentMode::chat);
}

// ===========================================================================
// Reject path: rejecting drops the plan and stays in plan mode
// ===========================================================================

TEST_CASE("reject_plan drops the plan and keeps mode=plan",
          "[integration][llm][plan]")
{
    auto& h = harness();
    h.agent().reset_conversation();
    h.agent().set_mode(AgentMode::plan);
    REQUIRE(wait_for_mode(h.agent(), AgentMode::plan));

    PromptResult r = h.prompt(
        "Use propose_plan to outline a SINGLE-step plan: "
        "step 1 = print 'hello' (tools_needed: []). Keep it short.",
        std::chrono::minutes(2));
    REQUIRE_FALSE(r.timed_out);

    auto plan = h.frontend().last_plan_proposed();
    if (!plan.has_value()) {
        WARN("Model did not call propose_plan; cannot exercise reject path.");
        h.agent().set_mode(AgentMode::chat);
        return;
    }
    REQUIRE(h.agent().current_plan().has_value());

    // Reject. Mode stays plan; current_plan clears.
    h.agent().reject_plan();
    // reject_plan is queued; the agent thread drains it on the next idle
    // cycle (queue_cv_ wakes immediately even without a message).
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!h.agent().current_plan().has_value()) break;
        std::this_thread::sleep_for(50ms);
    }
    CHECK_FALSE(h.agent().current_plan().has_value());
    CHECK(h.agent().mode() == AgentMode::plan);

    h.agent().set_mode(AgentMode::chat);
    (void)wait_for_mode(h.agent(), AgentMode::chat);
}
