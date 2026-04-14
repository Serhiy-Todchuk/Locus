#pragma once

#include "tool.h"

#include <string>
#include <string_view>

namespace locus {

// -- Enums --------------------------------------------------------------------

enum class ToolDecision { approve, reject, modify };

enum class CompactionStrategy {
    drop_tool_results,  // B: strip tool_result content from history
    drop_oldest         // C: remove N oldest turns
};

// -- IFrontend ----------------------------------------------------------------
// Core calls these on every registered frontend. Implementations must be
// thread-safe: callbacks fire on the agent thread.

class IFrontend {
public:
    virtual ~IFrontend() = default;

    // Streamed text token from LLM.
    virtual void on_token(std::string_view token) = 0;

    // A tool call needs approval. Frontend should display info and eventually
    // call ILocusCore::tool_decision(). For auto-approve tools this is NOT
    // called — the tool executes immediately and on_tool_result fires.
    virtual void on_tool_call_pending(const ToolCall& call,
                                      const std::string& preview) = 0;

    // Tool execution finished. display is the user-facing result text.
    virtual void on_tool_result(const std::string& call_id,
                                const std::string& display) = 0;

    // LLM finished its response (no more tokens or tool calls for this turn).
    virtual void on_message_complete() = 0;

    // Context window usage update.
    virtual void on_context_meter(int used_tokens, int limit) = 0;

    // Context is critically full. Frontend should offer compaction options.
    virtual void on_compaction_needed(int used_tokens, int limit) = 0;

    // Non-fatal error from the agent (LLM error, unknown tool, etc.).
    virtual void on_error(const std::string& message) = 0;
};

// -- ILocusCore ---------------------------------------------------------------
// Frontends call these into the Core.

class ILocusCore {
public:
    virtual ~ILocusCore() = default;

    virtual void register_frontend(IFrontend* fe) = 0;
    virtual void unregister_frontend(IFrontend* fe) = 0;

    // Send a user message. Kicks off an agent turn (may involve multiple
    // LLM round-trips if tools are called). Non-blocking in async mode;
    // blocks the calling thread in sync/CLI mode.
    virtual void send_message(const std::string& content) = 0;

    // Respond to an on_tool_call_pending event.
    virtual void tool_decision(const std::string& call_id,
                               ToolDecision decision,
                               const nlohmann::json& modified_args = {}) = 0;

    // Apply a compaction strategy to reduce context usage.
    virtual void compact_context(CompactionStrategy strategy, int n = 0) = 0;
};

} // namespace locus
