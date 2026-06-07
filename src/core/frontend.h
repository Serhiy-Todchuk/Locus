#pragma once

#include "activity_event.h"
#include "../agent/agent_mode.h"
#include "../llm/llm_client.h"   // MessageRole
#include "../tools/permission_presets.h"
#include "../tools/tool.h"

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

    // A tool call is starting. Always fired -- once per call -- regardless
    // of approval policy, so frontends can render the call in chat. When
    // `needs_approval` is true the frontend should additionally display the
    // approval UI and eventually call `ILocusCore::tool_decision()`. When
    // false the tool will execute immediately after this returns and
    // `on_tool_result` fires; frontends should NOT show approval UI.
    //
    // `safety_warnings` (S4.V Task 5) carries human-readable warnings the
    // dispatcher computed about this call -- currently the list of tokens
    // in `run_command` / `run_command_bg` arguments that reference paths
    // outside the workspace root. Frontends render these prominently in
    // the approval UI (yellow/red banner). When non-empty, the dispatcher
    // has already forced `needs_approval=true` even if the workspace
    // policy is auto_approve, so the user gets the chance to review.
    virtual void on_tool_call_pending(const ToolCall& call,
                                      const std::string& preview,
                                      bool needs_approval = true,
                                      const std::vector<std::string>& safety_warnings = {}) = 0;

    // Tool execution finished. display is the user-facing result text.
    // `success` is false for errors, denials, cancellations, and disabled
    // capabilities -- frontends use it to decide whether to render extras
    // like the S5.C inline diff (skipped on failure).
    virtual void on_tool_result(const std::string& call_id,
                                const std::string& display,
                                bool success = true) = 0;

    // Agent turn complete (no more tokens or tool calls). Fired once per
    // send_message, after all LLM round-trips finish. Pair with on_turn_start.
    virtual void on_turn_complete() = 0;

    // Context window usage update. `prompt_tokens` and `completion_tokens`
    // (S4.V Task 8) are the last server-reported split from the most recent
    // LLM round; 0 means "not yet reported" (e.g. session-open broadcast
    // before any LLM call). `reserve_tokens` (S5.D) is the headroom the
    // agent loop keeps free for the response; 0 means no reserve.
    // `stream_ms_last_round` is the wall-clock duration of the most recent
    // LLM stream call (0 when this broadcast was not triggered by a stream
    // completion -- session open, compaction, reset, etc.); paired with
    // completion_tokens it gives a per-bubble tok/s rate.
    // Frontends are free to ignore any of the extra params and render only
    // `used_tokens` -- the context meter still works.
    virtual void on_context_meter(int used_tokens, int limit,
                                  int prompt_tokens = 0,
                                  int completion_tokens = 0,
                                  int reserve_tokens = 0,
                                  long long stream_ms_last_round = 0) = 0;

    // Context is critically full. Frontend should offer compaction options.
    virtual void on_compaction_needed(int used_tokens, int limit) = 0;

    // S5.Z task 6 -- a pre-compaction history snapshot was just written.
    // `counter` is the monotonic 1-based archive number; the chat-footer
    // chip uses this to surface "compacted: N" without scraping disk.
    // S6.18 C.3 -- `no_op_count` is the running total of compactions that
    // returned reached=no (no archive written, no history change). The
    // chat-footer chip suffixes "(M no-op)" when > 0 so a stuck compaction
    // pipeline is visible at a glance without trawling .locus/locus.log.
    // Both default to legacy meaning for CLI / test frontends that ignore it.
    virtual void on_compaction_archived(int /*counter*/, int /*no_op_count*/ = 0) {}

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

    // An existing activity event was updated in place (same `id`), used by
    // ActivityLog when consecutive index_event entries are coalesced so the
    // activity panel doesn't pile up dozens of "Indexed 1 file" / "Embedding
    // progress: ..." rows. The `event` carries the current (post-update)
    // values; frontends should locate the row by id and replace it. Default
    // no-op so frontends that don't render the activity log keep compiling.
    virtual void on_activity_updated(const ActivityEvent& /*event*/) {}

    // Attached-context state changed (set or cleared). Empty when detached.
    // Frontends use this to show/hide a "📎 file" chip near the input.
    virtual void on_attached_context_changed(
        const std::optional<AttachedContext>& /*ctx*/) {}

    // -- Plan mode (S4.D) ----------------------------------------------------
    // Default no-op implementations so existing frontends keep compiling.

    // Agent's interaction mode changed (chat / plan / execute). Frontend
    // can update its mode-switcher UI and gate inputs accordingly.
    virtual void on_mode_changed(AgentMode /*mode*/) {}

    // Model called `propose_plan` and the dispatcher accepted the result.
    // Plan is awaiting the user's approve/reject decision.
    virtual void on_plan_proposed(const Plan& /*plan*/) {}

    // A step in the active plan transitioned (in_progress -> done|failed).
    // step_idx is 0-based.
    virtual void on_plan_step_advanced(const std::string& /*plan_id*/,
                                        int /*step_idx*/,
                                        PlanStep::Status /*status*/,
                                        const std::string& /*notes*/) {}

    // Every step is now done or failed. `success` is true if all steps
    // ended in `done`, false if any step ended in `failed`.
    virtual void on_plan_completed(const std::string& /*plan_id*/,
                                    bool /*success*/) {}

    // S4.L -- a per-turn auto-commit just landed in the user's git history.
    // `short_sha` is the abbreviated SHA, `branch` is the branch the commit
    // landed on, `subject` is the subject line as committed (already prefixed
    // and truncated). Frontends use this to flash a footer chip / activity
    // entry. Failure cases surface via the activity log + on_error path; this
    // callback fires only on success.
    virtual void on_auto_commit(const std::string& /*short_sha*/,
                                 const std::string& /*branch*/,
                                 const std::string& /*subject*/) {}

    // S4.F -- streaming generation progress. Fires from AgentLoop while a
    // stream is in flight, throttled to ~150 ms. `chars` is the total
    // accumulated stream bytes (visible + reasoning); `est_tokens` is a
    // ~4-chars-per-token estimate. Frontends use it to render a live token
    // counter next to the input. The post-turn `usage.completion_tokens` is
    // the authoritative count -- frontends should reconcile on
    // `on_turn_complete`. Default no-op so existing frontends keep compiling.
    virtual void on_generation_progress(int /*chars*/, int /*est_tokens*/) {}

    // Agentic Tetris findings #5 -- round progress signal. Fired at the top
    // of every AgentLoop round (after ++round, before the LLM POST). `round`
    // is 1-based; `max_rounds` is the per-message cap from WorkspaceConfig
    // (`agent.max_rounds_per_message`) -- 0 means "unbounded". Frontends use
    // this to render a "round N/M" chip while a turn is in flight so the
    // user can see whether the agent is making forward progress on long
    // build-fix loops vs. spinning. Default no-op so CLI / test stubs stay
    // clean. Paired with on_turn_complete: the chip is hidden then.
    virtual void on_round_progress(int /*round*/, int /*max_rounds*/) {}

    // S6.20 -- transient LLM-transport retry / waiting notice. Fired when the
    // endpoint returns a transient failure (429 / 5xx / empty body / stall) and
    // the transport is backing off before a retry. `status` is a short
    // human-readable string (e.g. "HTTP 429 (rate limited) -- retrying 2/5 in
    // 8s"). Frontends surface it in the chat-footer status line so a
    // multi-minute backoff on a flaky hosted endpoint isn't a silent hang; it
    // is naturally superseded by the next token / round / turn-complete event.
    // Default no-op so CLI / test stubs stay clean.
    virtual void on_llm_retry(const std::string& /*status*/) {}

    // S6.13 -- reasoning watchdog tripped on the current LLM round.
    // `trigger` is "chars" or "seconds"; `value` is the count that crossed
    // the threshold (combined reasoning+text chars for "chars"; elapsed
    // seconds for "seconds"). Frontends typically reveal a non-modal
    // "Commit now" button that calls ILocusCore::request_commit_now()
    // when clicked. In auto_nudge mode the action fires inside AgentCore
    // and the frontend just logs the event. Default no-op.
    virtual void on_reasoning_watchdog_tripped(const std::string& /*trigger*/,
                                                int /*value*/) {}

    // S6.13 -- watchdog state cleared (new round started, agent went idle,
    // or a tool call landed). Frontends hide the Commit-now button on this.
    // Default no-op.
    virtual void on_reasoning_watchdog_cleared() {}

    // S5.G -- a ChatMessage was just appended to ConversationHistory. Frontends
    // build their dom-id -> history-id map here so per-message delete can route
    // back to the right ConversationHistory entry. `role` is the message role
    // (user / assistant / system / tool); `deletable` is true for messages the
    // chat panel should let the user remove via the hover-reveal X (false for
    // system prompts, tool results paired with tool_call assistants, and any
    // other non-deletable shapes that AgentCore wants to keep). Default no-op.
    virtual void on_history_message_added(int /*history_id*/,
                                           MessageRole /*role*/,
                                           bool /*deletable*/) {}

    // S5.G -- a ChatMessage was removed from ConversationHistory by id. Fires
    // on every successful delete_by_id (including ones the chat panel didn't
    // initiate, so a CLI /forget-style hook would land here too). Frontends
    // remove the matching DOM bubble.
    virtual void on_history_message_deleted(int /*history_id*/) {}

    // S5.S -- effective permission preset changed. `effective` is the preset
    // the dispatcher will use for the next tool call (runtime override if
    // set, otherwise the detected workspace-config preset). `from_runtime` is
    // true when the change came from the chat-footer combobox (session-only),
    // false when it came from a settings save. Frontends use this to repaint
    // the bottom-bar chip. Default no-op so frontends that don't render the
    // chip stay clean.
    virtual void on_permission_preset_changed(
        tools::PermissionPreset /*effective*/,
        bool /*from_runtime*/) {}

    // S6.16 -- the active LLM endpoint changed (hot-swap completed on the
    // agent thread). `profile_name` is the now-active profile; `model` is the
    // resolved model id (server-detected or the profile default); `context_limit`
    // is the resolved window. Frontends repaint the chat-footer endpoint chip +
    // tooltip. Default no-op so CLI / test frontends compile clean.
    virtual void on_endpoint_changed(const std::string& /*profile_name*/,
                                     const std::string& /*model*/,
                                     int /*context_limit*/) {}
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

    // S6.13 -- "Commit now" -- cancel the in-flight LLM stream and start a
    // new round with a synthetic user message telling the model to stop
    // reasoning and either commit to a tool call or give a brief answer.
    // Distinct from cancel_turn (which aborts the whole turn); this keeps
    // the turn alive. No-op if not busy. Hard-capped at 2 nudges per turn;
    // a 3rd request aborts with "Agent appears stuck (3 reasoning watchdog
    // trips in one turn)." Default impl is a no-op so non-core frontends
    // (CLI, tests) don't have to implement it.
    virtual void request_commit_now() {}

    // Snapshot of buffered activity events with id > since_id. Used by
    // late-joining frontends (e.g. web clients) to catch up.
    virtual std::vector<ActivityEvent> get_activity(uint64_t since_id = 0) const = 0;

    // Roll back every file mutation from a single agent turn (S4.B).
    // turn_id == 0 means "the most recent turn that has a checkpoint".
    // Returns a human-readable summary of what was restored / skipped.
    virtual std::string undo_turn(int turn_id = 0) = 0;

    // Pin a workspace file to the conversation. The file's path + outline are
    // injected into the system prompt so the LLM is aware of it on every turn.
    // clear_attached_context() removes the pin.
    virtual void set_attached_context(AttachedContext ctx) = 0;
    virtual void clear_attached_context() = 0;
    virtual std::optional<AttachedContext> attached_context() const = 0;

    // -- Plan mode (S4.D) ----------------------------------------------------
    // Switch the agent's interaction mode. Queued onto the agent thread, so
    // safe to call from any frontend thread. A change from chat/execute back
    // to plan resets `current_plan` to empty.
    virtual void set_mode(AgentMode /*mode*/) {}

    // Current mode (snapshot; thread-safe).
    virtual AgentMode mode() const { return AgentMode::chat; }

    // Approve the most recently proposed plan. Switches mode to execute, pins
    // the plan to context, and prompts the agent to begin step 1.
    // No-op if no plan is currently awaiting approval.
    virtual void approve_plan() {}

    // Reject the most recently proposed plan. Returns to plan mode so the
    // user can re-prompt; current_plan is cleared.
    virtual void reject_plan() {}

    // S5.G -- per-message delete. Frontends call this with a stable history_id
    // (as broadcast via IFrontend::on_history_message_added). The agent core
    // routes the request onto its agent thread (so the ConversationOwnerScope
    // discipline holds) and broadcasts on_history_message_deleted on success.
    // Refuses the system message; refuses unknown ids. Default no-op so
    // implementations that don't support delete (test stubs) compile clean.
    virtual void delete_message(int /*history_id*/) {}

    // S5.S -- set or clear the runtime permission preset override. The
    // runtime override lives on the dispatcher and wins over the persisted
    // workspace config until cleared (passing custom -> nullopt). Settings
    // save calls clear_runtime_permission_preset() so the dialog's new
    // baseline takes over. Default no-op so test stubs compile.
    virtual void set_runtime_permission_preset(tools::PermissionPreset /*preset*/) {}
    virtual void clear_runtime_permission_preset() {}
    virtual std::optional<tools::PermissionPreset>
        runtime_permission_preset() const { return std::nullopt; }

    // S6.16 -- request an endpoint hot-swap to the named profile. Queued onto
    // the agent thread; applied at the next between-turns seam (deferred if a
    // turn is in flight). History is preserved; the next user message lands on
    // the new endpoint. Missing-profile requests warn and no-op. Default no-op
    // so non-core frontends compile clean.
    virtual void request_endpoint_switch(const std::string& /*profile_name*/) {}
};

} // namespace locus
