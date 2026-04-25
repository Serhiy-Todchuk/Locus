#pragma once

#include "activity_log.h"
#include "agent_loop.h"
#include "checkpoint_store.h"
#include "context_budget.h"
#include "conversation.h"
#include "file_change_tracker.h"
#include "metrics.h"
#include "core/workspace_services.h"
#include "frontend.h"
#include "frontend_registry.h"
#include "llm_client.h"
#include "session_manager.h"
#include "slash_commands.h"
#include "system_prompt.h"
#include "tool.h"
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
              const std::filesystem::path& checkpoints_dir = {});

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

private:
    void agent_thread_func();
    void process_message(const std::string& content);

    // Try to handle a /slash command (direct tool invocation). Returns true
    // if the input was a slash command. Delegates to slash_.
    bool try_slash_command(const std::string& content);

public:
    // Exposed so frontends can build autocomplete lists from the same
    // registry + parser the agent uses at dispatch time.
    SlashCommandDispatcher& slash_dispatcher() { return *slash_; }

private:

    // Compose system prompt from base + (optional) attached-context section.
    std::string compose_system_prompt() const;

    // Recompute system_prompt_ and overwrite the seed system message in
    // history_ in place. Called whenever the attached context changes.
    void refresh_system_prompt();

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

    // Sync-mode completion signal.
    std::mutex               sync_mutex_;
    std::condition_variable  sync_cv_;
    bool                     sync_turn_done_ = false;

    // Attached file context (S2.4).
    mutable std::mutex             attached_mutex_;
    std::optional<AttachedContext> attached_context_;
};

} // namespace locus
