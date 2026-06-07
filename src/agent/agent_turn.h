#pragma once

#include <string>

namespace locus {

class AgentCore;
struct ToolCall;

// AgentTurnRunner -- runs the multi-round LLM/tool loop for a single
// user-message turn. Extracted from AgentCore::process_message; reaches
// into AgentCore's private state via a friend declaration in agent_core.h.
//
// Lifetime: stack-allocated by process_message; one runner per turn.
// Threading: runs on the agent thread (the same thread as the rest of
// process_message), so any AgentCore atomics / mutexes that exist solely
// to fence non-agent threads against the loop are observed from the
// agent-thread side.
//
// The runner does not own any state of its own beyond a reference to the
// agent, the turn id, and the per-workspace round cap snapshot. Per-round
// counters live as locals inside run().
class AgentTurnRunner {
public:
    AgentTurnRunner(AgentCore& core, int turn_id, int max_rounds);

    struct Outcome {
        bool turn_had_error = false;
        int  rounds_run     = 0;  // last value of the round counter
    };

    Outcome run();

private:
    // S6.20 -- "stuck on the same build error" tracking. After each round's
    // tool results land, the runner distills any build-error diagnostics into
    // a signature (see build_error_detector.h). When the same non-empty
    // signature recurs `k_stuck_error_streak` rounds in a row we nudge once;
    // if it STILL recurs after the nudge, the turn aborts. Per-turn state, so
    // it resets naturally for the next user message.
    void check_stuck_error(const std::string& tool_output, bool& out_break);

    // S6.20 -- soft nudge toward edit_file when a LARGE full-file overwrite is
    // followed by a failed build in the same turn. Whole-file `write_file
    // overwrite` of a big file each round is the thrash signature (token-heavy,
    // error-reintroducing). Inspect a dispatched call; remember if it was a
    // large overwrite. When a build error then shows up, hint once.
    void note_write_call(const ToolCall& call);
    void maybe_hint_large_overwrite();

    static constexpr int    k_stuck_error_streak = 3;     // nudge on 3rd repeat
    static constexpr size_t k_large_overwrite_bytes = 16000;  // ~400 lines

    AgentCore&  core_;
    int         turn_id_;
    int         max_rounds_;  // 0 = unbounded
    std::string last_error_sig_;       // signature of the previous round's build error
    int         same_error_streak_ = 0;
    bool        stuck_nudge_fired_ = false;  // we already nudged for this streak
    bool        pending_large_overwrite_ = false;  // a big overwrite awaits a build result
    bool        large_overwrite_hinted_  = false;  // already hinted this turn (once only)
};

} // namespace locus
