#pragma once

#include "activity_event.h"
#include "tool.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace locus {

// -- Enums --------------------------------------------------------------------

enum class ToolDecision { approve, reject, modify };

enum class CompactionStrategy {
    drop_tool_results,  // B: strip tool_result content from history
    drop_oldest         // C: remove N oldest turns
};

// -- AttachedContext ----------------------------------------------------------
// A workspace-relative file the user has pinned to the conversation. When set,
// AgentCore injects "[Attached: <path>]" plus the file outline into the
// system prompt so the LLM is aware of it without any extra tool call.

struct AttachedContext {
    std::string file_path;   // workspace-relative
    std::string preview;     // optional one-line summary (currently unused by core)
};

// -- IFrontend ----------------------------------------------------------------
// Core calls these on every registered frontend. Implementations must be
// thread-safe: callbacks fire on the agent thread.

class IFrontend {
public:
    virtual ~IFrontend() = default;

    // Agent has begun processing a user message. Fired before the first LLM
    // call. Use for busy indicators and input disabling.
    virtual void on_turn_start() = 0;

    // Streamed text token from LLM (visible answer).
    virtual void on_token(std::string_view token) = 0;

    // Streamed chain-of-thought token (reasoning models). Default: ignore.
    virtual void on_reasoning_token(std::string_view /*token*/) {}

    // A tool call needs approval. Frontend should display info and eventually
    // call ILocusCore::tool_decision(). For auto-approve tools this is NOT
    // called — the tool executes immediately and on_tool_result fires.
    virtual void on_tool_call_pending(const ToolCall& call,
                                      const std::string& preview) = 0;

    // Tool execution finished. display is the user-facing result text.
    virtual void on_tool_result(const std::string& call_id,
                                const std::string& display) = 0;

    // Agent turn complete (no more tokens or tool calls). Fired once per
    // send_message, after all LLM round-trips finish. Pair with on_turn_start.
    virtual void on_turn_complete() = 0;

    // Context window usage update.
    virtual void on_context_meter(int used_tokens, int limit) = 0;

    // Context is critically full. Frontend should offer compaction options.
    virtual void on_compaction_needed(int used_tokens, int limit) = 0;

    // Conversation was reset (history cleared, system prompt re-seeded).
    virtual void on_session_reset() = 0;

    // Non-fatal error from the agent (LLM error, unknown tool, etc.).
    virtual void on_error(const std::string& message) = 0;

    // Embedding progress update (from background worker thread).
    virtual void on_embedding_progress(int done, int total) = 0;

    // Indexing progress update (fires during process_events batches).
    // Default no-op so frontends that don't care can ignore it.
    virtual void on_indexing_progress(int /*done*/, int /*total*/) {}

    // Activity log entry. All structured events (system prompts, user msgs,
    // LLM responses, tool calls, index events, warnings, errors) fan out
    // through this in addition to their specific callbacks. Frontends that
    // don't care can ignore it.
    virtual void on_activity(const ActivityEvent& event) = 0;

    // Attached-context state changed (set or cleared). Empty when detached.
    // Frontends use this to show/hide a "📎 file" chip near the input.
    virtual void on_attached_context_changed(
        const std::optional<AttachedContext>& /*ctx*/) {}
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

    // Reset conversation: clear history, re-seed system prompt, start fresh.
    virtual void reset_conversation() = 0;

    // Save the current conversation to a session file. Returns session ID.
    virtual std::string save_session() = 0;

    // Load a previously saved session, replacing current conversation.
    virtual void load_session(const std::string& session_id) = 0;

    // True while the agent is processing a turn (between on_turn_start and
    // on_turn_complete). Thread-safe.
    virtual bool is_busy() const = 0;

    // Request cancellation of the current turn. The agent will stop after
    // the current LLM call or tool execution finishes (not instant).
    // No-op if not busy.
    virtual void cancel_turn() = 0;

    // Snapshot of buffered activity events with id > since_id. Used by
    // late-joining frontends (e.g. web clients) to catch up.
    virtual std::vector<ActivityEvent> get_activity(uint64_t since_id = 0) const = 0;

    // Pin a workspace file to the conversation. The file's path + outline are
    // injected into the system prompt so the LLM is aware of it on every turn.
    // clear_attached_context() removes the pin.
    virtual void set_attached_context(AttachedContext ctx) = 0;
    virtual void clear_attached_context() = 0;
    virtual std::optional<AttachedContext> attached_context() const = 0;
};

} // namespace locus
