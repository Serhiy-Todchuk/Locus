#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace locus {

// Classification of activity events shown in the Details panel.
enum class ActivityKind {
    system_prompt,   // system prompt assembled / seeded
    tool_manifest,   // per-round tool catalog sent to the LLM (size + count)
    user_message,    // user turn submitted
    llm_response,    // LLM stream finished, carries usage
    tool_call,       // tool invocation started
    tool_result,     // tool invocation finished
    index_event,     // indexer / embedder progress
    memory_added,    // S4.R memory bank: new entry committed
    memory_searched, // S4.R memory bank: search_memory invoked
    memory_deleted,  // S4.R memory bank: entry removed
    compaction,      // M5 polish: context compaction queued / applied
    truncation_blocked,  // S6.10 Task G: write/edit refused -- truncation marker detected
    quality_correction,  // S6.10 Task B: detector fired and injected a corrective nudge
    warning,         // non-fatal warning
    error            // non-fatal error surfaced to user
};

// One line in the activity log. Events are append-only; `id` is
// monotonically assigned by AgentCore.
struct ActivityEvent {
    uint64_t                    id = 0;
    std::chrono::system_clock::time_point timestamp{};
    ActivityKind                kind = ActivityKind::warning;
    std::string                 summary;   // one line, shown in list
    std::string                 detail;    // full content, shown when expanded

    // Optional token accounting (populated for llm_response, system_prompt,
    // user_message). tokens_delta = change vs. previous turn's total.
    std::optional<int>          tokens_in;
    std::optional<int>          tokens_out;
    std::optional<int>          tokens_delta;
};

inline const char* to_string(ActivityKind k)
{
    switch (k) {
    case ActivityKind::system_prompt:   return "system_prompt";
    case ActivityKind::tool_manifest:   return "tool_manifest";
    case ActivityKind::user_message:    return "user_message";
    case ActivityKind::llm_response:    return "llm_response";
    case ActivityKind::tool_call:       return "tool_call";
    case ActivityKind::tool_result:     return "tool_result";
    case ActivityKind::index_event:     return "index_event";
    case ActivityKind::memory_added:    return "memory_added";
    case ActivityKind::memory_searched: return "memory_searched";
    case ActivityKind::memory_deleted:  return "memory_deleted";
    case ActivityKind::compaction:      return "compaction";
    case ActivityKind::truncation_blocked: return "truncation_blocked";
    case ActivityKind::quality_correction: return "quality_correction";
    case ActivityKind::warning:         return "warning";
    case ActivityKind::error:           return "error";
    }
    return "unknown";
}

} // namespace locus
