#pragma once

#include <string>
#include <vector>

namespace locus {

class AgentCore;
struct ToolCall;
struct AgentStepResult;

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

    // S6.21 Task 1 -- "all tool calls dropped as malformed" escalation. A round
    // where the stream delivered tool calls but ALL were dropped produces zero
    // tool results, so neither the S6.10 repeated-call monitor nor the S6.20
    // build-error streak fires. Read the per-round (delivered, dropped) counts
    // off the step result: nudge once on the first all-dropped round, abort the
    // turn on a second. A round with >=1 delivered call resets the streak.
    void check_all_dropped(const AgentStepResult& step, bool& out_break);

    // S6.21 Task 2 -- edit_file ambiguity / not-found loop. The per-result
    // remedy text (apply_single_edit) is the first line of defence; this is the
    // backstop when the model loops anyway. Count consecutive edit_file
    // ambiguity errors in this turn and inject the escape-hatch hint once on
    // the 2nd repeat (shared 2-cap).
    void check_edit_ambiguity(const std::string& tool_output);

    // S6.21 Task 3 -- after the turn's final assistant message lands, surface a
    // non-blocking tripwire if it claimed a verifiable build/run outcome with
    // no confirming tool call this round. Pure predicate in convergence_signals;
    // this only does the activity/footer side effect. `round_tool_names` are
    // the tool calls dispatched in the round that produced this message.
    void check_unverified_success(const std::string& assistant_text,
                                  const std::vector<std::string>& round_tool_names);

    // S6.21 Task 4 -- compaction breadcrumb. When AgentCore's hard-trim seam
    // fired this round AND the turn has a live build-error signature, prepend a
    // one-line "[Resuming: last build error was <sig>; you were editing <file>]"
    // user message so a 16k-class local model keeps the build-loop thread across
    // the trim. Reads-and-clears AgentCore::compaction_hard_trimmed_. No-op when
    // no hard-trim fired or no build error is live (pure-chat compaction is
    // unaffected). Gated by agent.quality_monitor_enabled.
    void maybe_inject_compaction_breadcrumb();

    static constexpr int    k_stuck_error_streak = 3;     // nudge on 3rd repeat
    static constexpr int    k_edit_ambiguity_nudge = 2;   // hint on 2nd repeat
    static constexpr size_t k_large_overwrite_bytes = 16000;  // ~400 lines

    AgentCore&  core_;
    int         turn_id_;
    int         max_rounds_;  // 0 = unbounded
    std::string last_error_sig_;       // signature of the previous round's build error
    int         same_error_streak_ = 0;
    bool        stuck_nudge_fired_ = false;  // we already nudged for this streak
    bool        pending_large_overwrite_ = false;  // a big overwrite awaits a build result
    bool        large_overwrite_hinted_  = false;  // already hinted this turn (once only)

    // S6.21 Task 1 -- all-dropped streak state. `all_dropped_nudged_` records
    // that we steered the current run of all-dropped rounds; cleared whenever a
    // delivered round resets the streak.
    bool        all_dropped_nudged_ = false;

    // S6.21 Task 2 -- consecutive edit_file ambiguity errors this turn + a
    // once-per-turn fired flag for the escape-hatch hint.
    int         edit_ambiguity_streak_ = 0;
    bool        edit_ambiguity_hinted_ = false;

    // S6.21 Task 4 -- last file the model passed to edit_file / write_file this
    // turn. The compaction breadcrumb names it ("you were editing <file>") so a
    // hard-trim mid-build-loop doesn't erase WHICH file was in flight.
    std::string last_edited_file_;
};

} // namespace locus
