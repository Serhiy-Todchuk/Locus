// S6.17 Task B -- Pass-5-replay regression test.
//
// Loads each of the four archived Pass-5 histories at
// `tests/ui_automation/output/agentic_TestLocalVibe2/history.before-compact-*.json`
// and runs the post-S6.17 compaction pipeline against them with the
// `balanced` aggressiveness preset. Asserts each one reaches the target
// (or at minimum shrinks dramatically) -- the regression guard for the F17
// finding ("compaction layers fail silently when there's nothing big to
// strip") that motivated the whole stage.
//
// The baseline pre-S6.17 results from .locus/locus.log for these four
// archives were:
//   #1: 9154 -> 9191 (target 9831, reached=yes)   -- already worked
//   #2: 10234 -> 10271 (target 9831, reached=no)  -- the F17 spiral
//   #3: 11343 -> 11380 (target 9831, reached=no)  -- the F17 spiral
//   #4: 12427 -> 12464 (target 9831, reached=no)  -- the F17 spiral
//
// Post-S6.17, all four must reach target.
//
// Manual-only -- tagged `[integration][s6.17][compaction-replay]`. The test
// uses the harness LLM client so the `layer_llm_summary` layer can fire;
// without it the deterministic layers (drop_redundant / strip_large /
// drop_old_reasoning) alone may not reach target on Pass-5's shape (every
// tool result was small, so Layer 2 had nothing to strip pre-summary).

#include "harness_fixture.h"

#include "agent/compaction_pipeline.h"
#include "agent/conversation.h"
#include "llm/llm_client.h"
#include "llm/token_counter.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace locus;
using namespace locus::integration;
namespace fs = std::filesystem;

namespace {

fs::path archive_dir()
{
    // LOCUS_INT_TEST_WORKSPACE_DIR is the repo root injected at CMake time.
    return fs::path(LOCUS_INT_TEST_WORKSPACE_DIR) /
           "tests" / "ui_automation" / "output" / "agentic_TestLocalVibe2";
}

ConversationHistory load_archive(int counter)
{
    fs::path p = archive_dir() / ("history.before-compact-" +
                                  std::to_string(counter) + ".json");
    REQUIRE(fs::exists(p));

    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    auto j = nlohmann::json::parse(ss.str());

    ConversationHistory hist;
    for (const auto& m : j["messages"])
        hist.add(ChatMessage::from_json(m));
    return hist;
}

// The Pass-5 baseline ran with effective_limit ~= 13108 and pipeline_target
// = round(13108 * 0.75) = 9831. We use the same target so the assertion
// reflects the original conditions.
constexpr int k_pass5_target_tokens = 9831;

} // namespace

TEST_CASE("S6.17 -- Pass-5-replay: archive #1 already reached pre-S6.17",
          "[integration][llm][s6.17][compaction-replay]")
{
    auto& h = harness();
    auto hist = load_archive(1);
    int before = hist.estimate_tokens();
    INFO("archive #1: " << before << " tokens before compaction");

    auto cfg  = WorkspaceConfig::Compaction{};
    cfg.aggressiveness = "balanced";
    auto sel = selection_from_config(cfg);

    auto result = CompactionPipeline::run(
        hist, sel, k_pass5_target_tokens, &h.llm(), {});
    REQUIRE(result.reached_target);
    REQUIRE(result.after_tokens <= k_pass5_target_tokens);
}

TEST_CASE("S6.17 -- Pass-5-replay: archive #2 F17 no-op spiral now reaches target",
          "[integration][llm][s6.17][compaction-replay]")
{
    // Baseline 2026-05-25: 10234 -> 10271 (target 9831, reached=no). The
    // tool results were small (60+60-line filtered run_command outputs) so
    // strip_large_tool_bodies found nothing. Post-S6.17 the balanced preset
    // brings in layer_llm_summary which doesn't depend on individual-body
    // size; the cascade should now reach target.
    auto& h = harness();
    auto hist = load_archive(2);
    int before = hist.estimate_tokens();
    INFO("archive #2: " << before << " tokens before compaction");

    auto cfg = WorkspaceConfig::Compaction{};
    cfg.aggressiveness = "balanced";
    auto sel = selection_from_config(cfg);

    auto result = CompactionPipeline::run(
        hist, sel, k_pass5_target_tokens, &h.llm(), {});

    INFO("archive #2 after: " << result.after_tokens
         << " (target " << k_pass5_target_tokens
         << ", reached=" << (result.reached_target ? "yes" : "no") << ")");
    // The structural fix is that the result MUST be substantially smaller
    // than `before + sentinel` (~before+37 was the baseline no-op pattern).
    REQUIRE(result.after_tokens < before);
    REQUIRE(result.reached_target);
}

TEST_CASE("S6.17 -- Pass-5-replay: archive #3 F17 no-op spiral now reaches target",
          "[integration][llm][s6.17][compaction-replay]")
{
    auto& h = harness();
    auto hist = load_archive(3);
    int before = hist.estimate_tokens();
    INFO("archive #3: " << before << " tokens before compaction");

    auto cfg = WorkspaceConfig::Compaction{};
    cfg.aggressiveness = "balanced";
    auto sel = selection_from_config(cfg);

    auto result = CompactionPipeline::run(
        hist, sel, k_pass5_target_tokens, &h.llm(), {});

    INFO("archive #3 after: " << result.after_tokens);
    REQUIRE(result.after_tokens < before);
    REQUIRE(result.reached_target);
}

TEST_CASE("S6.17 -- Pass-5-replay: archive #4 F17 no-op spiral now reaches target",
          "[integration][llm][s6.17][compaction-replay]")
{
    auto& h = harness();
    auto hist = load_archive(4);
    int before = hist.estimate_tokens();
    INFO("archive #4: " << before << " tokens before compaction");

    auto cfg = WorkspaceConfig::Compaction{};
    cfg.aggressiveness = "balanced";
    auto sel = selection_from_config(cfg);

    auto result = CompactionPipeline::run(
        hist, sel, k_pass5_target_tokens, &h.llm(), {});

    INFO("archive #4 after: " << result.after_tokens);
    REQUIRE(result.after_tokens < before);
    REQUIRE(result.reached_target);
}
