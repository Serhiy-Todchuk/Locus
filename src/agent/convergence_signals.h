#pragma once

// S6.21 -- pure convergence/honesty signals for the agent turn loop.
//
// A multi-model agentic session (TestLocalVibe4: gemma-4-31b / qwen3.7-max /
// qwen3-coder:free on OpenRouter + local gemma-4-12b-qat @ 16k) surfaced three
// loops the S6.10/S6.13/S6.19/S6.20 machinery doesn't cover, all of which boil
// down to a decision a pure function can make. Splitting them out here keeps
// AgentTurnRunner's side-effecting round loop thin and makes each decision
// unit-testable without a live turn (the same shape as build_error_detector.h):
//
//   - Task 1: a round where the stream delivered tool calls but ALL were
//     dropped as malformed produces ZERO tool results, so the S6.10 repeated-
//     call monitor (keys on identical name+args) and the S6.20 build-error
//     streak (keys on tool RESULTS) never fire. `classify_all_dropped`
//     decides nudge-then-abort from the per-round (delivered, dropped) counts.
//
//   - Task 2: edit_file "old_string matches N / not found" recurring within a
//     turn. `is_edit_ambiguity_error` recognizes the result body so the runner
//     can count repeats and inject the escape-hatch hint once.
//
//   - Task 3: the assistant asserting a verifiable outcome ("builds",
//     "frame0.ppm created", "the exe runs") in a round that ran NO confirming
//     tool call. `claims_unverified_success` is the conservative phrase +
//     zero-verifying-call predicate that drives the visibility tripwire.
//
// Pure + dependency-free (std::string / std::vector only) so all three
// unit-test without a workspace, LLM, or agent thread.

#include <string>
#include <vector>

namespace locus {

// ---- Task 1: all-tool-calls-dropped escalation -----------------------------

// What the runner should do this round given the malformed-call accounting.
enum class AllDroppedAction {
    none,    // not an all-dropped round (>=1 delivered, or no tool calls at all)
    nudge,   // first all-dropped round: steer the model once
    abort,   // a second all-dropped round after the nudge: give up on the turn
};

// Decide the action from this round's (delivered, dropped) tool-call counts and
// whether a nudge already fired for the current all-dropped streak. Pure: the
// caller owns the streak state (a single bool) and applies the side effects.
//
//   delivered>=1                 -> none  (resets the streak; caller clears it)
//   delivered==0 && dropped>=1   -> nudge on the first such round,
//                                   abort on the next one
//   delivered==0 && dropped==0   -> none  (no tool calls accumulated at all --
//                                   a plain text round; not an all-dropped loop)
//
// `already_nudged` is the runner's "we steered this streak" flag. The function
// does not mutate it; the runner sets it to true after a successful nudge and
// clears it when a delivered round resets the streak.
AllDroppedAction classify_all_dropped(int delivered, int dropped,
                                      bool already_nudged);

// ---- Task 2: edit_file ambiguity / not-found detection ---------------------

// True when `tool_output` is an edit_file failure of the ambiguous-match
// ("matches N locations") or not-found ("not found ... exact match required")
// kind -- the two cases that loop small models. Matches the strings
// apply_single_edit emits (S6.21 Task 2 self-healing remedies included).
// False for any other tool result (a successful edit, an unrelated error).
bool is_edit_ambiguity_error(const std::string& tool_output);

// ---- Task 3: unverified-success guard --------------------------------------

// True when `assistant_text` asserts a verifiable build/run outcome (a curated
// phrase list -- "builds", "compiles successfully", "frame0", "the exe runs",
// "screenshot created", ...) AND `this_round_tool_names` contains no tool that
// could have confirmed it (run_command / read_file / list_directory / search_*
// / get_file_outline). Conservative by construction: a turn that ran any
// verifying tool this round returns false, and the phrase set avoids generic
// summary words to keep false positives down. Drives a NON-blocking tripwire
// (no nudge, no abort) -- it tells the watching user the claim is ungrounded.
bool claims_unverified_success(const std::string& assistant_text,
                               const std::vector<std::string>& this_round_tool_names);

} // namespace locus
