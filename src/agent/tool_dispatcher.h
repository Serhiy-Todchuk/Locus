#pragma once

#include "core/workspace_services.h"
#include "frontend.h"
#include "frontend_registry.h"
#include "llm/llm_client.h"
#include "tool.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace locus {

class ActivityLog;
class CheckpointStore;
class FileChangeTracker;
class MetricsAggregator;

// Runs a single tool call through the approval gate, executes it, and
// injects the tool-result message back into conversation via the supplied
// append callback. Owns the decision condvar/mutex and is the sole reader
// of the shared cancel flag when blocked on approval.
//
// The dispatcher does not touch ConversationHistory directly — callers pass
// an `AppendFn` so the agent thread remains the only writer.
class ToolDispatcher {
public:
    using AppendFn = std::function<void(ChatMessage)>;

    ToolDispatcher(IToolRegistry& tools,
                   IWorkspaceServices& services,
                   ActivityLog& activity,
                   FrontendRegistry& frontends,
                   std::atomic<bool>& cancel_flag,
                   MetricsAggregator* metrics = nullptr);

    // Look up, gate, execute, and inject one tool call.
    // The unknown-tool case is handled here — a tool-error result is
    // appended and an error event is emitted.
    void dispatch(const ToolCall& call, const AppendFn& append_result);

    // Submit a user decision (called from any thread).
    void submit_decision(ToolDecision decision,
                         nlohmann::json modified_args = {});

    // Wake a dispatch that is blocked on approval (used by cancel).
    void wake();

    // Set the checkpoint context for the current turn. Called by AgentCore at
    // the top of each turn; cleared (store=nullptr) between turns. While set,
    // mutating tools (write_file / edit_file / delete_file) get a pre-mutation
    // snapshot before execute() runs.
    void set_turn_context(CheckpointStore* store,
                          std::string session_id,
                          int turn_id);

    // S4.T -- attach a file-change tracker so the dispatcher can mark paths
    // the agent itself mutated (and thus suppress them from next turn's
    // "files changed since last turn" notification). Lifetime is owned by
    // AgentCore; pass nullptr to disable.
    void set_change_tracker(FileChangeTracker* tracker);

private:
    // Inspect the tool name + args and snapshot the target path if relevant.
    void maybe_snapshot(const ToolCall& call);

    IToolRegistry&      tools_;
    IWorkspaceServices& services_;
    ActivityLog&        activity_;
    FrontendRegistry&   frontends_;
    std::atomic<bool>&  cancel_flag_;
    MetricsAggregator*  metrics_ = nullptr;

    std::mutex                  decision_mutex_;
    std::condition_variable     decision_cv_;
    std::optional<ToolDecision> pending_decision_;
    nlohmann::json              pending_modified_args_;

    // Per-turn checkpoint context (S4.B). Plain pointer because the store
    // lives at workspace scope; a null pointer disables checkpointing.
    CheckpointStore* checkpoints_     = nullptr;
    std::string      checkpoint_sid_;
    int              checkpoint_turn_ = 0;

    // S4.T -- file-change tracker (lives on AgentCore). When non-null, every
    // mutating tool call records the touched path so it gets suppressed from
    // next turn's external-changes notification.
    FileChangeTracker* change_tracker_ = nullptr;
};

} // namespace locus
