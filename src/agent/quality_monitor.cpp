#include "quality_monitor.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace locus {

namespace {

// Canonicalise a JSON arguments string so order-insensitive equality on the
// argument set survives field-shuffling. Returns the raw string when the
// input doesn't parse as JSON (defence in depth -- the production callers
// always feed us canonical bodies).
std::string canonical_args(const std::string& raw)
{
    try {
        auto j = nlohmann::json::parse(raw);
        return j.dump();  // dump() sorts keys deterministically.
    } catch (...) {
        return raw;
    }
}

// Find the index of the most recent assistant message in `messages`. Returns
// -1 when none exists.
int last_assistant_index(const std::vector<ChatMessage>& messages)
{
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (messages[i].role == MessageRole::assistant) return i;
    }
    return -1;
}

} // namespace

std::optional<QualityCorrection> QualityMonitor::evaluate(
    const std::vector<ChatMessage>& messages)
{
    int latest_idx = last_assistant_index(messages);
    if (latest_idx < 0) return std::nullopt;

    const ChatMessage& latest = messages[latest_idx];

    // -- empty_response ------------------------------------------------------
    // No visible text, no tool call. Reasoning-only assistants happen in
    // healthy thinking-model flows pre-tool-call; the watchdog (S6.13) is
    // the right surface for those. Empty + empty is the failure mode here.
    if (latest.content.empty() && latest.tool_calls.empty()
        && latest.reasoning_content.empty())
    {
        QualityCorrection c;
        c.kind    = QualityCorrectionKind::empty_response;
        c.summary = "Empty response detected";
        c.corrective_message =
            "[Quality monitor] Your previous response was empty. Respond "
            "with either text or a tool call.";
        return c;
    }

    // -- repeated_tool_call --------------------------------------------------
    // Latest assistant called >=1 tool. Walk backwards from latest-1 to find
    // the previous assistant message that ALSO carries tool_calls -- if it
    // matches (name + canonical args) on the first call, the model is in a
    // tight loop and a corrective is warranted.
    if (latest.tool_calls.empty()) return std::nullopt;
    const auto& latest_call = latest.tool_calls.front();

    for (int i = latest_idx - 1; i >= 0; --i) {
        const auto& m = messages[i];
        if (m.role != MessageRole::assistant) continue;
        if (m.tool_calls.empty()) continue;

        const auto& prev_call = m.tool_calls.front();
        if (prev_call.name != latest_call.name) return std::nullopt;
        if (canonical_args(prev_call.arguments)
                != canonical_args(latest_call.arguments))
            return std::nullopt;

        // Match -- fire.
        QualityCorrection c;
        c.kind    = QualityCorrectionKind::repeated_tool_call;
        c.summary = "Repeated tool call: " + latest_call.name;
        c.corrective_message =
            "[Quality monitor] You just called `" + latest_call.name +
            "` with the same arguments and it did not produce new "
            "information. Try a different approach: different arguments, a "
            "different tool, or ask the user for clarification.";
        return c;
    }

    return std::nullopt;
}

} // namespace locus
