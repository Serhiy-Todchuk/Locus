#pragma once

#include "llm/llm_client.h"

#include <optional>
#include <string>
#include <vector>

namespace locus {

// S6.10 Task B -- detector layer over `ConversationHistory` that catches two
// failure modes specific to small local models:
//
//   - empty_response       -- assistant message has no content, no tool_calls,
//                             and no reasoning surfaced into text.
//   - repeated_tool_call   -- the just-issued tool call repeats the previous
//                             tool call's (name, args) verbatim.
//
// The detector is stateless: each call examines the supplied history slice
// and returns at most one correction. The two failure modes the original
// spec listed -- `malformed_args` and `unknown_tool` -- are intentionally
// NOT covered here: Task A's repair pre-pass + the ToolDispatcher try/catch
// fence already surface a useful error to the model for the first; the
// dispatcher's existing unknown-tool branch handles the second (with a
// closest-match suggestion added by this same stage).
//
// Wiring lives on AgentCore: after a tool call has executed (or after an
// assistant turn yields with no tool call), AgentCore calls evaluate() and,
// on a Some(correction) result, threads the message through the existing
// nudges_this_turn_ + synthetic-user-message path that S6.13 already built
// for the reasoning watchdog. The 2-nudge cap and "Agent appears stuck"
// abort therefore apply uniformly -- no separate counter.

enum class QualityCorrectionKind {
    empty_response,
    repeated_tool_call,
};

struct QualityCorrection {
    QualityCorrectionKind kind;
    std::string           corrective_message;  // synthetic user message body
    std::string           summary;             // one-line activity-log summary
};

class QualityMonitor {
public:
    // Inspect the conversation tail and decide whether a corrective nudge
    // should fire. Returns std::nullopt when the latest assistant turn
    // looks healthy. Pure function -- safe to call from any thread.
    //
    // The caller is expected to have just appended the latest assistant
    // message (and any tool-result messages that followed it); evaluate
    // looks back from `messages.back()` to find the most recent assistant
    // turn + the one before it.
    static std::optional<QualityCorrection> evaluate(
        const std::vector<ChatMessage>& messages);
};

} // namespace locus
