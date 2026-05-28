// S6.13 -- reasoning watchdog unit tests.
//
// Coverage:
// - WorkspaceConfig round-trips reasoning_max_seconds / reasoning_max_chars /
//   reasoning_max_rounds_silent / reasoning_auto_nudge through JSON.
// - JSON round-trip preserves both default (0/false = off) and explicit values.
// - S6.18 D.2: evaluate_watchdog() decision matrix -- silent-stream trip,
//   text-only commit-phase skip, reasoning-channel-progress skip, and the
//   genuinely-silent reasoning stream (zero per-tick delta) still trips on
//   the seconds budget.
//
// AgentLoop-level watchdog trip behaviour requires an end-to-end live LLM
// (covered by the manual run protocol in the stage doc). The AgentCore
// nudge dispatch is exercised by the live integration suite.

#include "agent/agent_loop.h"
#include "core/workspace.h"
#include "core/workspace_config_json.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using locus::WorkspaceConfig;
using locus::workspace_config_to_json;
using locus::workspace_config_from_json;

}  // namespace

TEST_CASE("reasoning_watchdog: defaults are off", "[s6.13][reasoning_watchdog]")
{
    WorkspaceConfig cfg;
    REQUIRE(cfg.agent.reasoning_max_seconds == 0);
    REQUIRE(cfg.agent.reasoning_max_chars == 0);
    REQUIRE(cfg.agent.reasoning_max_rounds_silent == 0);
    REQUIRE(cfg.agent.reasoning_auto_nudge == false);
}

TEST_CASE("reasoning_watchdog: JSON round-trip preserves values",
          "[s6.13][reasoning_watchdog]")
{
    WorkspaceConfig src;
    src.agent.reasoning_max_seconds       = 120;
    src.agent.reasoning_max_chars         = 30000;
    src.agent.reasoning_max_rounds_silent = 5;
    src.agent.reasoning_auto_nudge        = true;

    auto j = workspace_config_to_json(src);
    REQUIRE(j["agent"]["reasoning_max_seconds"]       == 120);
    REQUIRE(j["agent"]["reasoning_max_chars"]         == 30000);
    REQUIRE(j["agent"]["reasoning_max_rounds_silent"] == 5);
    REQUIRE(j["agent"]["reasoning_auto_nudge"]        == true);

    WorkspaceConfig restored = workspace_config_from_json(j);
    REQUIRE(restored.agent.reasoning_max_seconds       == 120);
    REQUIRE(restored.agent.reasoning_max_chars         == 30000);
    REQUIRE(restored.agent.reasoning_max_rounds_silent == 5);
    REQUIRE(restored.agent.reasoning_auto_nudge        == true);
}

TEST_CASE("reasoning_watchdog: missing keys yield default-constructed values",
          "[s6.13][reasoning_watchdog]")
{
    nlohmann::json j = {{"agent", nlohmann::json::object()}};
    WorkspaceConfig cfg = workspace_config_from_json(j);
    REQUIRE(cfg.agent.reasoning_max_seconds       == 0);
    REQUIRE(cfg.agent.reasoning_max_chars         == 0);
    REQUIRE(cfg.agent.reasoning_max_rounds_silent == 0);
    REQUIRE(cfg.agent.reasoning_auto_nudge        == false);
}

// -- S6.18 D.2: evaluate_watchdog() decision matrix ---------------------------

namespace {
locus::WatchdogDecisionInputs make_inputs()
{
    locus::WatchdogDecisionInputs in;
    in.wd_max_chars   = 20000;
    in.wd_max_seconds = 120;
    in.recent_progress = true;
    return in;
}
} // namespace

TEST_CASE("evaluate_watchdog: budgets off -> never trips",
          "[s6.18][reasoning_watchdog][evaluate_watchdog]")
{
    auto in = make_inputs();
    in.wd_max_chars   = 0;
    in.wd_max_seconds = 0;
    in.reasoning_chars = 1'000'000;
    in.elapsed_seconds = 1'000'000;
    REQUIRE(locus::evaluate_watchdog(in).empty());
}

TEST_CASE("evaluate_watchdog: chars budget trips when exceeded",
          "[s6.18][reasoning_watchdog][evaluate_watchdog]")
{
    auto in = make_inputs();
    in.reasoning_chars = 25000;       // > 20000
    in.recent_progress = false;       // stalled
    REQUIRE(locus::evaluate_watchdog(in) == "chars");
}

TEST_CASE("evaluate_watchdog: seconds budget trips on silent stream",
          "[s6.18][reasoning_watchdog][evaluate_watchdog]")
{
    // Silent stream: 200s elapsed, no chars accumulated (chunks never
    // arrived). The timer thread's wd_max_seconds path must still fire.
    auto in = make_inputs();
    in.text_chars      = 0;
    in.reasoning_chars = 0;
    in.elapsed_seconds = 200;
    in.recent_progress = false;
    REQUIRE(locus::evaluate_watchdog(in) == "seconds");
}

TEST_CASE("evaluate_watchdog: S6.17 C.2 text-only commit-phase skip",
          "[s6.18][reasoning_watchdog][evaluate_watchdog]")
{
    // Final-answer commit shape: model is streaming plain text, no
    // reasoning channel, no tool_calls yet. Should NOT trip even if the
    // chars budget is technically exceeded.
    auto in = make_inputs();
    in.text_chars      = 25000;       // > 20000
    in.reasoning_chars = 0;
    in.tool_call_seen  = false;
    REQUIRE(locus::evaluate_watchdog(in).empty());

    // But seconds budget still bites if the stream is actually stuck.
    in.elapsed_seconds = 200;
    REQUIRE(locus::evaluate_watchdog(in) == "seconds");
}

TEST_CASE("evaluate_watchdog: S6.18 D.1 reasoning-channel-progress skip",
          "[s6.18][reasoning_watchdog][evaluate_watchdog]")
{
    // Reasoning-channel-heavy local model (Qwen 3.6-27B shape):
    // reasoning_chars is climbing, no tool call yet, timer tick observed
    // positive delta -> chars budget suppressed.
    auto in = make_inputs();
    in.reasoning_chars = 25000;       // > 20000
    in.text_chars      = 0;
    in.tool_call_seen  = false;
    in.recent_progress = true;
    REQUIRE(locus::evaluate_watchdog(in).empty());

    // Once recent_progress flips to false WITH chars also exceeded, the
    // D.1 skip is disarmed and chars-budget trips immediately -- this is
    // the "genuinely stuck reasoning stream" path.
    in.recent_progress = false;
    in.elapsed_seconds = 30;
    REQUIRE(locus::evaluate_watchdog(in) == "chars");

    // Genuinely-silent reasoning stream below the chars cap but past
    // wd_max_seconds: chars trigger inactive, seconds trigger fires.
    in.reasoning_chars = 5000;  // < wd_max_chars
    in.recent_progress = false;
    in.elapsed_seconds = 200;
    REQUIRE(locus::evaluate_watchdog(in) == "seconds");
}

TEST_CASE("evaluate_watchdog: D.1 does NOT suppress once a tool_call is seen",
          "[s6.18][reasoning_watchdog][evaluate_watchdog]")
{
    // After the model has committed a tool_call, the round is no longer in
    // "deliberation" mode -- a runaway chars budget IS a genuine problem
    // even with positive delta.
    auto in = make_inputs();
    in.reasoning_chars = 25000;
    in.tool_call_seen  = true;
    in.recent_progress = true;
    REQUIRE(locus::evaluate_watchdog(in) == "chars");
}

TEST_CASE("evaluate_watchdog: D.1 does NOT suppress when reasoning_chars is zero",
          "[s6.18][reasoning_watchdog][evaluate_watchdog]")
{
    // text-only stream past the chars budget without the C.2 floor met
    // (text_chars < 100). D.1 specifically gates on reasoning_chars > 0;
    // a chatty text stream that's NOT in commit-phase shape should still
    // trip.
    auto in = make_inputs();
    in.text_chars      = 25000;
    in.reasoning_chars = 0;
    in.tool_call_seen  = true;       // disables C.2 commit-phase skip
    in.recent_progress = true;
    REQUIRE(locus::evaluate_watchdog(in) == "chars");
}
