#pragma once

namespace locus {

class AgentCore;

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
    AgentCore& core_;
    int        turn_id_;
    int        max_rounds_;  // 0 = unbounded
};

} // namespace locus
