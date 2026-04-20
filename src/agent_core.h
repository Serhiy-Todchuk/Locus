#pragma once

#include "conversation.h"
#include "frontend.h"
#include "frontend_registry.h"
#include "llm_client.h"
#include "session_manager.h"
#include "system_prompt.h"
#include "tool.h"

#include <atomic>
#include <condition_variable>
#include <functional>
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
              const WorkspaceContext& ws_context,
              const std::string& locus_md,
              const WorkspaceMetadata& ws_meta,
              const LLMConfig& llm_config,
              const std::filesystem::path& sessions_dir);

    ~AgentCore() override;

    AgentCore(const AgentCore&) = delete;
    AgentCore& operator=(const AgentCore&) = delete;

    // -- ILocusCore -----------------------------------------------------------

    void register_frontend(IFrontend* fe) override;
    void unregister_frontend(IFrontend* fe) override;

    // Non-blocking: enqueues message, returns immediately.
    void send_message(const std::string& content) override;

    // Blocking: waits until the turn completes. Used by CLI frontend.
    void send_message_sync(const std::string& content);

    void tool_decision(const std::string& call_id,
                       ToolDecision decision,
                       const nlohmann::json& modified_args = {}) override;

    void compact_context(CompactionStrategy strategy, int n = 0) override;
    void reset_conversation() override;

    std::string save_session() override;
    void load_session(const std::string& session_id) override;

    bool is_busy() const override;
    void cancel_turn() override;

    // Direct access to session manager for listing etc.
    SessionManager& sessions() { return sessions_; }

    // -- Accessors ------------------------------------------------------------

    const ConversationHistory& history() const { return history_; }
    int context_limit() const { return llm_config_.context_limit; }

    // Start/stop the agent thread. Must be called after construction.
    // stop() waits for the current turn to finish before returning.
    void start();
    void stop();

    // Late-joining frontend catch-up: returns all buffered events with
    // id > since_id.
    std::vector<ActivityEvent> get_activity(uint64_t since_id = 0) const override;

    // External subsystems (indexer, embedding worker) report activity here.
    // Thread-safe. Broadcasts to all frontends and appends to ring buffer.
    void emit_index_event(const std::string& summary,
                          const std::string& detail = {});

private:
    // Build + broadcast + buffer a new activity event.
    void emit_activity(ActivityKind kind,
                       std::string summary,
                       std::string detail = {},
                       std::optional<int> tokens_in = std::nullopt,
                       std::optional<int> tokens_out = std::nullopt,
                       std::optional<int> tokens_delta = std::nullopt);

    // Agent thread entry point: drains the message queue.
    void agent_thread_func();

    // Process a single user message (runs on agent thread).
    void process_message(const std::string& content);

    // Try to handle a /slash command (direct tool invocation).
    // Returns true if handled, false if not a command.
    bool try_slash_command(const std::string& content);

    // Run one LLM call, stream tokens, detect tool calls.
    // Returns true if tool calls were made (loop should continue).
    bool run_llm_step();

    // Process a single tool call: approval gate -> execute -> inject result.
    void process_tool_call(const ToolCall& call, ITool* tool);

    // Build the OpenAI tools array from the registry.
    std::vector<ToolSchema> build_tool_schemas() const;

    // Check if adding est_new_tokens would overflow context. If at hard limit,
    // fires on_compaction_needed and returns true (caller should stop).
    bool check_context_overflow();

    // Best-effort token count: uses server-reported usage if available,
    // otherwise falls back to heuristic estimate.
    int current_token_count() const;

    // Members
    ILLMClient&      llm_;
    IToolRegistry&   tools_;
    WorkspaceContext  ws_context_;
    LLMConfig        llm_config_;
    ConversationHistory history_;
    std::string      system_prompt_;

    // Last server-reported token usage (0 if server doesn't report it).
    int last_server_total_tokens_ = 0;

    FrontendRegistry frontends_;
    SessionManager   sessions_;

    // Tool decision synchronization.
    std::mutex              decision_mutex_;
    std::condition_variable decision_cv_;
    std::optional<ToolDecision> pending_decision_;
    nlohmann::json          pending_modified_args_;

    // Agent thread and message queue.
    std::thread              agent_thread_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_cv_;
    std::queue<std::string>  message_queue_;
    std::atomic<bool>        running_{false};
    std::atomic<bool>        busy_{false};
    std::atomic<bool>        cancel_requested_{false};

    // Sync-mode: signaled when a turn completes.
    std::mutex               sync_mutex_;
    std::condition_variable  sync_cv_;
    bool                     sync_turn_done_ = false;

    // Compaction threshold (fraction of context_limit).
    static constexpr double k_compaction_threshold = 0.80;

    // Activity ring buffer (protected by activity_mutex_).
    mutable std::mutex         activity_mutex_;
    std::vector<ActivityEvent> activity_buffer_;
    uint64_t                   next_activity_id_ = 1;
    int                        prev_turn_total_tokens_ = 0;
    static constexpr size_t k_activity_buffer_max = 1000;
};

} // namespace locus
