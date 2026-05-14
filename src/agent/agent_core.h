#pragma once

#include "activity_log.h"
#include "agent_loop.h"
#include "agent_mode.h"
#include "checkpoint_store.h"
#include "context_budget.h"
#include "conversation.h"
#include "file_change_tracker.h"
#include "metrics.h"
#include "core/workspace_services.h"
#include "../core/frontend.h"
#include "../core/frontend_registry.h"
#include "llm/llm_client.h"
#include "prompt_templates.h"
#include "session_manager.h"
#include "slash_commands.h"
#include "system_prompt.h"
#include "../tools/tool.h"
#include "tool_dispatcher.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace locus {

// AgentCore orchestrates the conversation loop:
//   build prompt -> call LLM -> stream tokens -> detect tool calls ->
//   pause for approval -> execute -> inject result -> resume
//
// Since S3.A the heavy lifting lives in collaborators (AgentLoop,
// ToolDispatcher, ActivityLog, ContextBudget); AgentCore is the thread
// owner + queue pump + lifecycle coordinator.
//
// Threading model (S1.1):
//   The agent loop runs on a dedicated thread. send_message() is non-blocking:
//   it enqueues the message and returns immediately. All IFrontend callbacks
//   fire on the agent thread. tool_decision() may be called from any thread.
//
// For CLI compatibility, send_message_sync() blocks the caller until the
// turn completes (same as the M0 behavior).

class AgentCore : public ILocusCore {
public:
    AgentCore(ILLMClient& llm,
              IToolRegistry& tools,
              IWorkspaceServices& services,
              const std::string& locus_md,
              const WorkspaceMetadata& ws_meta,
              const LLMConfig& llm_config,
              const std::filesystem::path& sessions_dir,
              const std::filesystem::path& checkpoints_dir = {},
              const std::filesystem::path& project_prompts_dir = {},
              const std::filesystem::path& global_prompts_dir  = {});

    ~AgentCore() override;

    AgentCore(const AgentCore&) = delete;
    AgentCore& operator=(const AgentCore&) = delete;

    // -- ILocusCore -----------------------------------------------------------

    void register_frontend(IFrontend* fe) override;
    void unregister_frontend(IFrontend* fe) override;

    void send_message(const std::string& content) override;

    // Blocking variant — waits until the turn completes (CLI).
    void send_message_sync(const std::string& content);

    void tool_decision(const std::string& call_id,
                       ToolDecision decision,
                       const nlohmann::json& modified_args = {}) override;

    void compact_context(CompactionStrategy strategy, int n = 0) override;
    void reset_conversation() override;

    std::string save_session() override;
    void load_session(const std::string& session_id) override;

    // -- Checkpoint / undo (S4.B) --------------------------------------------
    // Inspect the per-session checkpoint history (for UI listings).
    std::vector<TurnInfo> list_checkpoints() const;

    const std::string& current_session_id() const { return session_id_; }
    int current_turn_id() const { return turn_id_; }

    // S5.C -- read the pre-mutation snapshot of a workspace-relative path for
    // the current turn's checkpoint. Returns nullopt when no snapshot exists
    // for that path (e.g. the file didn't exist before write_file, or the
    // snapshot was skipped because the file was too large), or when the
    // checkpoint store isn't active. Used by the chat panel to render
    // `write_file` / `delete_file` diffs against the pre-mutation state.
    std::optional<std::string> read_current_pre_mutation(
        const std::string& rel_path) const;

    bool is_busy() const override;
    void cancel_turn() override;

    std::string undo_turn(int turn_id = 0) override;

    // Export the current metrics snapshot to a file in `.locus/metrics/`.
    // `format` is "json" or "csv". Returns the absolute path on success or
    // an error string. Used by the GUI menu and the /export_metrics slash.
    std::string export_metrics(const std::string& format) const;

    SessionManager&    sessions() { return sessions_; }
    IToolRegistry&     tools()    { return tools_; }
    MetricsAggregator& metrics()  { return *metrics_; }

    const ConversationHistory& history() const { return history_; }
    int context_limit() const { return llm_config_.context_limit; }

    // Start/stop the agent thread. Must be called after construction.
    // stop() waits for the current turn to finish before returning.
    void start();
    void stop();

    std::vector<ActivityEvent> get_activity(uint64_t since_id = 0) const override;

    // External subsystems (indexer, embedding worker) report activity here.
    void emit_index_event(const std::string& summary,
                          const std::string& detail = {});

    // -- Attached file context (S2.4) ----------------------------------------
    void set_attached_context(AttachedContext ctx) override;
    void clear_attached_context() override;
    std::optional<AttachedContext> attached_context() const override;

    // -- Plan mode (S4.D) ----------------------------------------------------
    void       set_mode(AgentMode m) override;
    AgentMode  mode() const override;
    void       approve_plan() override;
    void       reject_plan() override;

    // Snapshot of the current plan, if any. Thread-safe.
    std::optional<Plan> current_plan() const;

    // S4.F -- compose the volatile-tail attached-file block that is prepended
    // to the next user message. Public so tests can verify the content
    // without driving an LLM round-trip. Returns "" when nothing is attached.
    std::string attached_context_block() const { return compose_attached_context_block(); }

private:
    void agent_thread_func();
    void process_message(const std::string& content);

    // S4.X -- the three outcomes for `try_slash_command`. Built-ins +
    // tool-slashes + unknown-command + help all collapse to `handled`; a
    // prompt-template hit emits `expanded` with `expanded_text` carrying
    // the substituted body so `process_message` can run it as a user
    // message; everything else (non-slash text) is `not_a_slash`.
    enum class SlashKind { not_a_slash, handled, expanded };
    struct SlashOutcome {
        SlashKind   kind = SlashKind::not_a_slash;
        std::string expanded_text;
    };

    SlashOutcome try_slash_command(const std::string& content);

public:
    // Exposed so frontends can build autocomplete lists from the same
    // registry + parser the agent uses at dispatch time.
    SlashCommandDispatcher& slash_dispatcher() { return *slash_; }

    // Exposed so the GUI can populate the autocomplete list with templates
    // and trigger reloads from a settings panel. Null when no prompts dirs
    // were configured (tests / very minimal sessions).
    PromptTemplateRegistry* prompt_templates() { return prompt_templates_.get(); }

private:

    // Compose the volatile-tail block injected on the next user message for
    // the currently attached file (S4.F: this used to live in the system
    // prompt; moving it to the user-message tail keeps the system prompt
    // byte-stable across turns so LM Studio / llama.cpp's prefix cache fires).
    // Returns an empty string when no file is attached.
    std::string compose_attached_context_block() const;

    // Service a pending compaction request on the agent thread. Caller must
    // already hold the conversation owner scope. No-op when the flag is
    // clear. Called from agent_thread_func before/after process_message
    // and from the round loop in process_message between LLM rounds so
    // mid-turn compactions take effect on the next request.
    void apply_pending_compaction();

    // Service a pending mode change request on the agent thread. Caller must
    // hold the conversation owner scope. No-op when the flag is clear.
    void apply_pending_mode_change();

    // Service a pending approve_plan / reject_plan request. Same threading
    // requirements as apply_pending_mode_change.
    void apply_pending_plan_decision();

    // Inspect a just-completed tool message to detect propose_plan and
    // mark_step_done invocations and update plan state accordingly. Called
    // from process_message's tool-dispatch loop, on the agent thread,
    // immediately after dispatcher_->dispatch returns. The result content
    // is the JSON string the tool returned (echoed plan or step status).
    void observe_plan_tool_result(const ToolCall& call,
                                  const std::string& result_content,
                                  bool result_success);

    // Current token count = budget_.current(history_.estimate_tokens()).
    int current_token_count() const;

    // Members
    ILLMClient&         llm_;
    IToolRegistry&      tools_;
    IWorkspaceServices& services_;
    LLMConfig           llm_config_;
    ConversationHistory history_;
    std::string         base_system_prompt_;
    std::string         system_prompt_;

    FrontendRegistry frontends_;
    SessionManager   sessions_;

    std::unique_ptr<ActivityLog>           activity_;
    std::unique_ptr<ContextBudget>         budget_;
    std::unique_ptr<MetricsAggregator>     metrics_;
    std::unique_ptr<AgentLoop>             loop_;
    std::unique_ptr<ToolDispatcher>        dispatcher_;
    std::unique_ptr<SlashCommandDispatcher> slash_;
    std::unique_ptr<PromptTemplateRegistry> prompt_templates_;
    std::unique_ptr<CheckpointStore>       checkpoints_;
    // S4.T -- snapshots indexed file mtimes between turns. Always allocated;
    // notification is gated by WorkspaceConfig::notify_external_changes at
    // injection time.
    std::unique_ptr<FileChangeTracker>     change_tracker_;

    // Session identity for the checkpoint store. Generated at construction;
    // overwritten by save_session()/load_session() so saved-and-resumed
    // sessions keep their checkpoint dir.
    std::string session_id_;
    // Monotonic turn counter — bumped before each user message goes to the
    // LLM. Used as the on-disk turn folder name.
    int         turn_id_ = 0;

    // Agent thread + message queue.
    std::thread              agent_thread_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_cv_;
    std::queue<std::string>  message_queue_;
    std::atomic<bool>        running_{false};
    std::atomic<bool>        busy_{false};
    std::atomic<bool>        cancel_requested_{false};

    // Compaction request from a non-agent thread (GUI). The mutator on
    // ConversationHistory carries an owner-thread fence (S3.I), so direct
    // mutation from a foreign thread asserts. The agent thread services
    // this request between rounds (or while idle) under its owner scope.
    std::atomic<bool>            pending_compact_{false};
    std::atomic<CompactionStrategy> pending_compact_strategy_{
        CompactionStrategy::drop_tool_results};
    std::atomic<int>             pending_compact_n_{0};

    // Sync-mode completion signal.
    std::mutex               sync_mutex_;
    std::condition_variable  sync_cv_;
    bool                     sync_turn_done_ = false;

    // Attached file context (S2.4).
    mutable std::mutex             attached_mutex_;
    std::optional<AttachedContext> attached_context_;

    // -- Plan mode (S4.D) -----------------------------------------------------
    // Mode is mutated on the agent thread only (via queued requests, like
    // pending_compact_). Read by frontends via the public accessor under a
    // mutex. The atomic gives a cheap fast-path read for AgentLoop's
    // build_tool_schemas call without locking.
    std::atomic<AgentMode> mode_{AgentMode::chat};

    // Pending mode change requested by a non-agent thread. Same pattern as
    // pending_compact_: stored as an atomic, drained at safe points on the
    // agent thread.
    std::atomic<bool>      pending_mode_change_{false};
    std::atomic<AgentMode> pending_mode_{AgentMode::chat};

    // Pending plan-decision requests from the GUI: approve_plan / reject_plan.
    // Drained on the agent thread alongside pending_mode_change_.
    std::atomic<bool>      pending_plan_approve_{false};
    std::atomic<bool>      pending_plan_reject_{false};

    // The active plan (set when propose_plan fires and accepted by the
    // dispatcher; cleared on reject / completion / mode reset). Owned by
    // the agent thread; the public accessor takes plan_mutex_ to copy out.
    mutable std::mutex     plan_mutex_;
    std::optional<Plan>    current_plan_;
    int                    plan_seq_ = 0;   // monotonic id source
    // True between propose_plan and approve/reject -- the plan is awaiting
    // the user's decision and the agent is paused (no further LLM rounds).
    bool                   plan_awaiting_decision_ = false;
};

} // namespace locus
