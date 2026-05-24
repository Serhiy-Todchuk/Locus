// S6.13 -- reasoning watchdog unit tests.
//
// Coverage:
// - WorkspaceConfig round-trips reasoning_max_seconds / reasoning_max_chars /
//   reasoning_max_rounds_silent / reasoning_auto_nudge through JSON.
// - JSON round-trip preserves both default (0/false = off) and explicit values.
//
// AgentLoop-level watchdog trip behaviour requires an end-to-end live LLM
// (covered by the manual run protocol in the stage doc). The AgentCore
// nudge dispatch is exercised by the live integration suite.

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
    REQUIRE(cfg.reasoning_max_seconds == 0);
    REQUIRE(cfg.reasoning_max_chars == 0);
    REQUIRE(cfg.reasoning_max_rounds_silent == 0);
    REQUIRE(cfg.reasoning_auto_nudge == false);
}

TEST_CASE("reasoning_watchdog: JSON round-trip preserves values",
          "[s6.13][reasoning_watchdog]")
{
    WorkspaceConfig src;
    src.reasoning_max_seconds       = 120;
    src.reasoning_max_chars         = 30000;
    src.reasoning_max_rounds_silent = 5;
    src.reasoning_auto_nudge        = true;

    auto j = workspace_config_to_json(src);
    REQUIRE(j["agent"]["reasoning_max_seconds"]       == 120);
    REQUIRE(j["agent"]["reasoning_max_chars"]         == 30000);
    REQUIRE(j["agent"]["reasoning_max_rounds_silent"] == 5);
    REQUIRE(j["agent"]["reasoning_auto_nudge"]        == true);

    WorkspaceConfig restored = workspace_config_from_json(j);
    REQUIRE(restored.reasoning_max_seconds       == 120);
    REQUIRE(restored.reasoning_max_chars         == 30000);
    REQUIRE(restored.reasoning_max_rounds_silent == 5);
    REQUIRE(restored.reasoning_auto_nudge        == true);
}

TEST_CASE("reasoning_watchdog: missing keys yield default-constructed values",
          "[s6.13][reasoning_watchdog]")
{
    nlohmann::json j = {{"agent", nlohmann::json::object()}};
    WorkspaceConfig cfg = workspace_config_from_json(j);
    REQUIRE(cfg.reasoning_max_seconds       == 0);
    REQUIRE(cfg.reasoning_max_chars         == 0);
    REQUIRE(cfg.reasoning_max_rounds_silent == 0);
    REQUIRE(cfg.reasoning_auto_nudge        == false);
}
