#pragma once

#include "conversation.h"
#include "frontend.h"
#include "llm_client.h"
#include "system_prompt.h"
#include "tool.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace locus {

// AgentCore orchestrates the conversation loop:
//   build prompt -> call LLM -> stream tokens -> detect tool calls ->
//   pause for approval -> execute -> inject result -> resume
//
// Threading: send_message() blocks the calling thread until the full turn
// completes (including all tool call round-trips). Callbacks (on_token, etc.)
// fire on the calling thread. tool_decision() may be called from any thread
// (it signals the agent loop via condition variable).
//
// For S0.7 CLI: main thread calls send_message(), which calls
// on_tool_call_pending() synchronously. The CLI frontend reads stdin in that
// callback and calls tool_decision() before returning, so the loop continues
// on the same thread without actual multi-thread synchronization.

class AgentCore : public ILocusCore {
public:
    AgentCore(ILLMClient& llm,
              IToolRegistry& tools,
              const WorkspaceContext& ws_context,
              const std::string& locus_md,
              const WorkspaceMetadata& ws_meta,
              const LLMConfig& llm_config);

    ~AgentCore() override = default;

    AgentCore(const AgentCore&) = delete;
    AgentCore& operator=(const AgentCore&) = delete;

    // -- ILocusCore -----------------------------------------------------------

    void register_frontend(IFrontend* fe) override;
    void unregister_frontend(IFrontend* fe) override;

    // Blocks until the turn completes (text-only response from LLM).
    void send_message(const std::string& content) override;

    void tool_decision(const std::string& call_id,
                       ToolDecision decision,
                       const nlohmann::json& modified_args = {}) override;

    void compact_context(CompactionStrategy strategy, int n = 0) override;

    // -- Accessors ------------------------------------------------------------

    const ConversationHistory& history() const { return history_; }
    int context_limit() const { return llm_config_.context_limit; }

private:
    // Run one LLM call, stream tokens, detect tool calls.
    // Returns true if tool calls were made (loop should continue).
    bool run_llm_step();

    // Process a single tool call: approval gate -> execute -> inject result.
    void process_tool_call(const ToolCall& call, ITool* tool);

    // Build the OpenAI tools array from the registry.
    std::vector<ToolSchema> build_tool_schemas() const;

    // Broadcast helpers.
    void broadcast_token(std::string_view token);
    void broadcast_tool_call_pending(const ToolCall& call, const std::string& preview);
    void broadcast_tool_result(const std::string& call_id, const std::string& display);
    void broadcast_message_complete();
    void broadcast_context_meter();
    void broadcast_compaction_needed();
    void broadcast_error(const std::string& msg);

    // Check if adding est_new_tokens would overflow context. If at hard limit,
    // fires on_compaction_needed and returns true (caller should stop).
    bool check_context_overflow();

    // Members
    ILLMClient&      llm_;
    IToolRegistry&   tools_;
    WorkspaceContext  ws_context_;
    LLMConfig        llm_config_;
    ConversationHistory history_;
    std::string      system_prompt_;

    std::vector<IFrontend*> frontends_;
    std::mutex              frontends_mutex_;

    // Tool decision synchronization.
    std::mutex              decision_mutex_;
    std::condition_variable decision_cv_;
    std::optional<ToolDecision> pending_decision_;
    nlohmann::json          pending_modified_args_;

    // Compaction threshold (fraction of context_limit).
    static constexpr double k_compaction_threshold = 0.80;
};

} // namespace locus
