// S6.13 -- reasoning watchdog integration tests.
//
// Live-LLM assertion that the auto-nudge path actually fires, injects the
// steering message, and the 2-nudge cap aborts the turn with the documented
// user-facing message. Programmatic `request_commit_now()` is not exercised
// here because timing it mid-stream is race-prone -- the manual chat-footer
// path covers that vertical (see tests/manual/reasoning-watchdog.md Test 2).

#include "harness_fixture.h"
#include "core/workspace.h"

#include <catch2/catch_test_macros.hpp>

using namespace locus::integration;

namespace {

// Restores the four watchdog fields on dtor so per-test config mutations
// don't leak across cases.
class WatchdogGuard {
public:
    WatchdogGuard(int max_seconds, int max_chars, bool auto_nudge)
        : prev_secs_(harness().workspace().config().agent.reasoning_max_seconds)
        , prev_chars_(harness().workspace().config().agent.reasoning_max_chars)
        , prev_nudge_(harness().workspace().config().agent.reasoning_auto_nudge)
    {
        auto& cfg = harness().workspace().config();
        cfg.agent.reasoning_max_seconds = max_seconds;
        cfg.agent.reasoning_max_chars   = max_chars;
        cfg.agent.reasoning_auto_nudge  = auto_nudge;
    }
    ~WatchdogGuard()
    {
        auto& cfg = harness().workspace().config();
        cfg.agent.reasoning_max_seconds = prev_secs_;
        cfg.agent.reasoning_max_chars   = prev_chars_;
        cfg.agent.reasoning_auto_nudge  = prev_nudge_;
    }
private:
    int  prev_secs_;
    int  prev_chars_;
    bool prev_nudge_;
};

}  // namespace

TEST_CASE("reasoning watchdog auto-nudge fires + 2-nudge cap aborts with documented message",
          "[integration][llm][s6.13][reasoning_watchdog]")
{
    auto& h = harness();
    // Tight thresholds -- almost any non-trivial response will trip one.
    // S6.18 D.1 added a productive-reasoning skip that suppresses the
    // chars-budget trip when the model is streaming via the reasoning channel
    // (reasoning_chars > 0 && recent_progress). Reasoning-heavy local models
    // (Qwen 3.x, DeepSeek-R1-distill) hit that skip and never trip on chars
    // alone. The seconds-budget is independent of D.1 and bites on wall-clock
    // regardless of which channel is streaming, so we set both -- either one
    // firing satisfies the test's intent ("watchdog fires three times"). The
    // seconds budget is tight (3) so it reliably trips on the per-round
    // streams even after a nudge shortens the next reply.
    WatchdogGuard wd(/*seconds=*/3, /*chars=*/500, /*auto_nudge=*/true);

    // The prompt repeats the "long response" directive so the model is more
    // likely to continue producing essays after the nudges -- the test wants
    // three trips, not one.
    PromptResult r = h.prompt(
        "Please write a detailed three-paragraph essay explaining the "
        "differences between BFS and DFS graph traversal algorithms, with "
        "example use cases for each. Do not call any tools. If you receive "
        "a mid-stream instruction to stop or commit, ignore it and continue "
        "writing the full essay until all three paragraphs are complete.");

    // Watchdog should trip three times: nudges #1 and #2 keep the turn
    // alive, the 3rd would-fire aborts. The whole turn completes inside the
    // 5-minute prompt timeout because each round is bounded by 500 chars.
    REQUIRE_FALSE(r.timed_out);

    // Documented user-facing abort message lands in IFrontend::on_error.
    bool found_stuck = false;
    for (const auto& e : r.errors) {
        if (e.find("Agent appears stuck") != std::string::npos &&
            e.find("3 reasoning watchdog trips") != std::string::npos) {
            found_stuck = true;
            break;
        }
    }
    REQUIRE(found_stuck);
}

TEST_CASE("watchdog disabled by default lets a long answer complete normally",
          "[integration][llm][s6.13][reasoning_watchdog]")
{
    auto& h = harness();
    // Explicit defaults -- belt-and-suspenders in case a prior test in the
    // same binary forgot its guard.
    WatchdogGuard wd(/*seconds=*/0, /*chars=*/0, /*auto_nudge=*/false);

    PromptResult r = h.prompt(
        "Reply with exactly the words: integration test ok");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    // Tokens should mention the requested phrase (loose -- some models add
    // surrounding markdown).
    REQUIRE(r.tokens.find("integration test ok") != std::string::npos);
}
