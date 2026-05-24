#pragma once

#include "conversation.h"
#include "llm/llm_client.h"

#include <cstddef>
#include <string>
#include <vector>

namespace locus {

// S5.F -- Composable compaction. The pipeline runs a fixed-order cascade of
// layers (1 -> 2 -> 3 -> 5 -> 6); each layer runs only when the previous ones
// haven't freed enough room to bring usage under `target_tokens`. Layer 4
// (drop low-priority tagged messages) is parked along with S5.E.
//
// Auto-preservation heuristics (replace the parked S5.E explicit pin states):
//   - system message [0]
//   - first user message of the session
//   - the most recent `keep_recent_turns` turn pairs
//   - any message with an `[Attached file: ...]` block
//   - short user messages (content <= preserve_short_user_msgs_max_tokens)
//   - short tool-call assistant messages
//     (content <= preserve_short_tool_calls_max_tokens)
//   - assistant messages whose tool_calls include `propose_plan` or
//     `mark_step_done` (any size) -- the plan structure is shared working
//     memory across many turns; dropping it orphans the LLM's view of a
//     plan that AgentCore still tracks server-side
// A threshold of 0 on either short-msg knob disables that heuristic.

// Layer toggles + per-layer knobs. AgentCore snapshots WorkspaceConfig into
// this struct so a config edit mid-cascade can't change behaviour.
struct CompactionLayerSelection {
    bool drop_redundant_tool_results = true;   // Layer 1
    bool strip_large_tool_bodies     = true;   // Layer 2
    bool drop_old_reasoning          = true;   // Layer 3
    bool drop_oldest_turns           = false;  // Layer 5 (escalation)
    bool llm_summary                 = true;   // Layer 6

    int  strip_threshold_tokens                = 1000;
    int  older_than_turns                      = 3;
    int  keep_recent_turns                     = 3;
    int  preserve_short_user_msgs_max_tokens   = 500;
    int  preserve_short_tool_calls_max_tokens  = 500;
    int  summary_max_tokens                    = 1024;
    std::string custom_summary_instructions;
};

// Per-layer result. `name` is human-readable; `tokens_freed` is the
// pre-layer minus post-layer estimate; `messages_touched` counts how many
// history entries the layer rewrote/dropped. `summary_text` is populated
// only by Layer 6 (the LLM summary the layer inserted).
struct LayerResult {
    std::string name;
    bool        ran             = false;
    int         tokens_freed    = 0;
    int         messages_touched = 0;
    std::string summary_text;
    std::string detail;          // optional one-line human-readable explanation
};

struct PipelineResult {
    int                      before_tokens = 0;
    int                      after_tokens  = 0;
    int                      target_tokens = 0;
    std::vector<LayerResult> layers;
    bool                     reached_target = false;
};

class CompactionPipeline {
public:
    // Run the cascade against `history`. Stops as soon as
    // history.estimate_tokens() <= target_tokens. `llm` is required only when
    // Layer 6 fires; pass nullptr to skip the summary layer outright.
    // `llm_config` is forwarded into the summary call (model / context_limit
    // are read for hint text; sampler knobs aren't used).
    static PipelineResult run(ConversationHistory& history,
                              const CompactionLayerSelection& cfg,
                              int target_tokens,
                              ILLMClient* llm,
                              const LLMConfig& llm_config);

    // ---- Helpers reused by the dialog preview ----------------------------

    // Returns true when this message must NOT be dropped/touched by Layers 5/6
    // under the auto-preservation heuristics.
    static bool is_preserved(const std::vector<ChatMessage>& msgs,
                             std::size_t idx,
                             const CompactionLayerSelection& cfg,
                             std::size_t first_user_idx,
                             std::size_t keep_recent_from);

    // Compute the index ranges Layer 5 / Layer 6 would consider as drop
    // candidates (oldest turn-groups that contain no preserved message).
    struct DropSpan {
        std::size_t begin = 0;  // inclusive
        std::size_t end   = 0;  // exclusive
    };
    static std::vector<DropSpan> drop_candidates(
        const std::vector<ChatMessage>& msgs,
        const CompactionLayerSelection& cfg);

    // Compose the prepended summary message text used by Layer 6 from the
    // free-form summary returned by the LLM.
    static std::string format_summary_message(const std::string& summary,
                                              std::size_t turn_span);
};

} // namespace locus
