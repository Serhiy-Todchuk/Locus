#pragma once

#include "core/workspace_services.h"
#include "../core/frontend.h"
#include "../core/frontend_registry.h"
#include "llm/llm_client.h"
#include "../tools/tool.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

    // Submit a user decision (called from any thread). Keyed by `call_id`
    // (S5.O) so a stale or late-delivered decision can never wake a tool
    // call the user did not approve. If no dispatch is currently waiting on
    // `call_id` the decision is held in the map for a brief window and
    // dropped with a warn log when the matching call never arrives.
    void submit_decision(const std::string& call_id,
                         ToolDecision decision,
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

    // S5.O -- per-call decision map. A dispatch in flight waits on the cv
    // until its own `call.id` lands in `pending_decisions_`; any decision
    // submitted for a call_id not currently being waited on is dropped with
    // a warn-log (stale UI click after the call already completed). Only
    // ids in `awaiting_dispatches_` are considered live waiters; the set is
    // populated when `dispatch` enters the wait and erased after consume.
    struct PendingDecision {
        ToolDecision   decision;
        nlohmann::json modified_args;
    };
    std::mutex                                       decision_mutex_;
    std::condition_variable                          decision_cv_;
    std::unordered_map<std::string, PendingDecision> pending_decisions_;
    std::unordered_set<std::string>                  awaiting_dispatches_;

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
