#pragma once

#include "activity_log.h"
#include "agent_loop.h"
#include "agent_mode.h"
#include "checkpoint_store.h"
#include "compaction_pipeline.h"
#include "context_budget.h"
#include "conversation.h"
#include "file_change_tracker.h"
#include "history_archive.h"
#include "llm_context.h"
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
// S5.J split the responsibility cleanly: the conversation's full LLM-facing
// state (history, system prompt, budget, file-change tracker, attached
// context, checkpoints, session/turn ids) lives on `LLMContext` and AgentCore
// is the agent-loop runner that composes one. Plan-mode state, agent thread,
// message queue, frontends, and metrics still live on AgentCore.
//
// Threading model (S1.1, S3.I):
//   The agent loop runs on a dedicated thread. send_message() is non-blocking:
//   it enqueues the message and returns immediately. All IFrontend callbacks
//   fire on the agent thread. tool_decision() may be called from any thread.
//   ConversationOwnerScope (S3.I) gates LLMContext mutations to the agent
//   thread during each turn; between turns the fence is cleared so inter-turn
//   operations (CLI REPL /reset etc.) run on any thread.

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

    // Blocking variant -- waits until the turn completes (CLI).
    void send_message_sync(const std::string& content);

    void tool_decision(const std::string& call_id,
                       ToolDecision decision,
                       const nlohmann::json& modified_args = {}) override;

    void compact_context(CompactionStrategy strategy, int n = 0) override;

    // S5.F -- composable compaction. Run the layer cascade with the given
    // selection and per-run target. `target_tokens` defaults to 0 = "drop
    // under warn_threshold of effective_limit" using the workspace config.
    // Per-run `custom_instructions_override` replaces the workspace's
    // `compaction.custom_summary_instructions` for this run only (used by
    // the `/compact <instructions>` slash command). Queued onto the agent
    // thread; returns immediately.
    void run_compaction(const CompactionLayerSelection& selection,
                        int target_tokens = 0,
                        const std::string& custom_instructions_override = "");

    void reset_conversation() override;

    // S5.G -- per-message delete. Queues onto the agent thread so the
    // ConversationOwnerScope discipline holds during the actual delete.
    // Refuses the system message (delegated to ConversationHistory).
    void delete_message(int history_id) override;

    // S5.S -- runtime permission preset override (chat-footer combobox path).
    // Stored on the dispatcher; broadcast via on_permission_preset_changed.
    // Custom is rejected silently; pass the explicit clear method below.
    void set_runtime_permission_preset(tools::PermissionPreset preset) override;
    void clear_runtime_permission_preset() override;
    std::optional<tools::PermissionPreset> runtime_permission_preset() const override;

    // Compute the preset the dispatcher will use right now: runtime override
    // if set, otherwise the detected workspace-config preset. Used by
    // frontends to repaint the chip when they attach mid-session.
    tools::PermissionPreset effective_permission_preset() const;

    // Broadcast a fresh on_permission_preset_changed event derived from the
    // current state. Called by LocusFrame after a settings save (the panel
    // may have updated tool_approval_policies, which changes the detected
    // preset even though no runtime override is in play).
    void rebroadcast_permission_preset(bool from_runtime = false);

    std::string save_session() override;
    void load_session(const std::string& session_id) override;

    // -- Checkpoint / undo (S4.B) --------------------------------------------
    std::vector<TurnInfo> list_checkpoints() const;

    const std::string& current_session_id() const { return ctx_->session_id(); }
    int current_turn_id() const { return ctx_->turn_id(); }

    // S5.C -- read the pre-mutation snapshot of a workspace-relative path
    // for the current turn's checkpoint.
    std::optional<std::string> read_current_pre_mutation(
        const std::string& rel_path) const;

    bool is_busy() const override;
    void cancel_turn() override;
    void request_commit_now() override;

    std::string undo_turn(int turn_id = 0) override;

    std::string export_metrics(const std::string& format) const;

    SessionManager&    sessions() { return sessions_; }
    IToolRegistry&     tools()    { return tools_; }
    MetricsAggregator& metrics()  { return *metrics_; }

    // S5.J -- direct access to the LLM-view state owner. Frontends that used
    // to peek at `agent.history()` keep working via this accessor (the legacy
    // history() accessor is preserved below as a passthrough).
    LLMContext&       context()       { return *ctx_; }
    const LLMContext& context() const { return *ctx_; }

    const ConversationHistory& history() const { return ctx_->history(); }
    int context_limit() const { return ctx_->llm_config().context_limit; }

    // S5.Z task 6 -- total number of pre-compaction snapshots ever written for
    // a session id. Used by the chat footer to resync its "compacted: N" chip
    // when a saved session is loaded. Returns 0 if no archive has been written.
    int compacted_archive_count(const std::string& session_id) const;

    // Start/stop the agent thread. Must be called after construction.
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

    std::optional<Plan> current_plan() const;

    // S4.F -- compose the volatile-tail attached-file block that is prepended
    // to the next user message.
    std::string attached_context_block() const { return ctx_->compose_attached_context_block(); }

private:
    void agent_thread_func();
    void process_message(const std::string& content);

    enum class SlashKind { not_a_slash, handled, expanded };
    struct SlashOutcome {
        SlashKind   kind = SlashKind::not_a_slash;
        std::string expanded_text;
    };

    SlashOutcome try_slash_command(const std::string& content);

public:
    SlashCommandDispatcher& slash_dispatcher() { return *slash_; }

    PromptTemplateRegistry* prompt_templates() { return prompt_templates_.get(); }

private:
    void apply_pending_compaction();
    void apply_pending_mode_change();
    void apply_pending_plan_decision();
    void apply_pending_deletes();

    void observe_plan_tool_result(const ToolCall& call,
                                  const std::string& result_content,
                                  bool result_success);

    // S6.10 Task B -- inject a synthetic-user-message nudge, share the
    // S6.13 per-turn 2-cap with the reasoning watchdog. Returns false when
    // the cap would be exceeded (caller should abort the turn with the
    // standard "Agent appears stuck" error). Quality monitor uses this;
    // future detector layers can hook the same entry point.
    bool inject_nudge(std::string body, const std::string& reason_tag);

    int current_token_count() const { return ctx_->current_tokens(); }

    // Members
    ILLMClient&         llm_;
    IToolRegistry&      tools_;
    IWorkspaceServices& services_;

    FrontendRegistry frontends_;
    SessionManager   sessions_;

    // S5.J -- LLM-view state owner. Created in the ctor body after we've
    // probed the workspace metadata and assembled the system prompt.
    std::unique_ptr<LLMContext>             ctx_;

    std::unique_ptr<ActivityLog>            activity_;
    std::unique_ptr<MetricsAggregator>      metrics_;
    std::unique_ptr<AgentLoop>              loop_;
    std::unique_ptr<ToolDispatcher>         dispatcher_;
    std::unique_ptr<SlashCommandDispatcher> slash_;
    std::unique_ptr<PromptTemplateRegistry> prompt_templates_;

    // Agent thread + message queue.
    std::thread              agent_thread_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_cv_;
    std::queue<std::string>  message_queue_;
    std::atomic<bool>        running_{false};
    std::atomic<bool>        busy_{false};
    std::atomic<bool>        cancel_requested_{false};

    // S6.13 -- "Commit now" requested (by UI button or by the AgentLoop's
    // auto-nudge path). Set in tandem with cancel_requested_ so the round
    // loop can distinguish "user wanted Stop" (turn aborts) from "watchdog
    // wanted Commit now" (turn continues with an injected steering message).
    // `nudges_this_turn_` is the per-turn counter; reset at the top of
    // process_message; capped at 2 nudges before the turn force-aborts.
    std::atomic<bool>        nudge_requested_{false};
    int                      nudges_this_turn_ = 0;

    // Compaction request from a non-agent thread (GUI).
    std::atomic<bool>            pending_compact_{false};
    std::atomic<CompactionStrategy> pending_compact_strategy_{
        CompactionStrategy::drop_tool_results};
    std::atomic<int>             pending_compact_n_{0};

    // S5.F -- new pipeline request (set when run_compaction() is called from
    // any thread). Protected by queue_mutex_.
    bool                     pending_pipeline_{false};
    CompactionLayerSelection pending_pipeline_selection_;
    int                      pending_pipeline_target_  = 0;
    std::string              pending_pipeline_overrides_;

    // S5.G -- pending per-message delete ids, queued from any thread, drained
    // on the agent thread before each turn (and between turns when the queue
    // is otherwise idle). Protected by queue_mutex_.
    std::vector<int>         pending_delete_ids_;

    // S5.F -- pre-compaction history archive. One per session, owned by
    // AgentCore so the archive lifetime matches the agent.
    std::unique_ptr<HistoryArchive> history_archive_;

    // Sync-mode completion signal.
    std::mutex               sync_mutex_;
    std::condition_variable  sync_cv_;
    bool                     sync_turn_done_ = false;

    // -- Plan mode (S4.D) -----------------------------------------------------
    std::atomic<AgentMode> mode_{AgentMode::chat};
    std::atomic<bool>      pending_mode_change_{false};
    std::atomic<AgentMode> pending_mode_{AgentMode::chat};
    std::atomic<bool>      pending_plan_approve_{false};
    std::atomic<bool>      pending_plan_reject_{false};
    mutable std::mutex     plan_mutex_;
    std::optional<Plan>    current_plan_;
    int                    plan_seq_ = 0;
    bool                   plan_awaiting_decision_ = false;
};

} // namespace locus
