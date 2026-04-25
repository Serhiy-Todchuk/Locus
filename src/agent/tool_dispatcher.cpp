#include "tool_dispatcher.h"

#include "activity_log.h"
#include "checkpoint_store.h"
#include "tools/shared.h"
#include "workspace.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace locus {

ToolDispatcher::ToolDispatcher(IToolRegistry& tools,
                               IWorkspaceServices& services,
                               ActivityLog& activity,
                               FrontendRegistry& frontends,
                               std::atomic<bool>& cancel_flag)
    : tools_(tools)
    , services_(services)
    , activity_(activity)
    , frontends_(frontends)
    , cancel_flag_(cancel_flag)
{}

void ToolDispatcher::submit_decision(ToolDecision decision,
                                     nlohmann::json modified_args)
{
    std::lock_guard lock(decision_mutex_);
    pending_decision_ = decision;
    pending_modified_args_ = std::move(modified_args);
    decision_cv_.notify_one();
}

void ToolDispatcher::wake()
{
    decision_cv_.notify_all();
}

void ToolDispatcher::set_turn_context(CheckpointStore* store,
                                      std::string session_id,
                                      int turn_id)
{
    checkpoints_     = store;
    checkpoint_sid_  = std::move(session_id);
    checkpoint_turn_ = turn_id;
}

void ToolDispatcher::maybe_snapshot(const ToolCall& call)
{
    if (!checkpoints_ || checkpoint_sid_.empty()) return;

    // Map tool name -> action label. Only mutating tools snapshot.
    std::string action;
    if      (call.tool_name == "edit_file")   action = "edit";
    else if (call.tool_name == "write_file")  action = "write";
    else if (call.tool_name == "delete_file") action = "delete";
    else                                      return;

    std::string rel = call.args.value("path", "");
    if (rel.empty()) return;

    auto full = tools::resolve_path(services_, rel);
    if (full.empty()) return;  // outside workspace; tool will reject too

    checkpoints_->snapshot_before(checkpoint_sid_, checkpoint_turn_,
                                  services_.root(), full, action);
}

void ToolDispatcher::dispatch(const ToolCall& call, const AppendFn& append_result)
{
    auto* tool = tools_.find(call.tool_name);

    if (!tool) {
        spdlog::warn("ToolDispatcher: unknown tool '{}'", call.tool_name);
        ChatMessage err;
        err.role = MessageRole::tool;
        err.tool_call_id = call.id;
        err.content = "Error: unknown tool '" + call.tool_name + "'";
        append_result(std::move(err));
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_error("Unknown tool: " + call.tool_name);
        });
        activity_.emit(ActivityKind::error,
                       "Unknown tool: " + call.tool_name,
                       "LLM requested tool '" + call.tool_name +
                           "' which is not registered.");
        return;
    }

    spdlog::info("ToolDispatcher: tool call '{}' id={}", call.tool_name, call.id);
    spdlog::trace("ToolDispatcher: tool args: {}", call.args.dump());

    activity_.emit(ActivityKind::tool_call,
                   "Tool call: " + call.tool_name,
                   "id: " + call.id + "\nargs: " + call.args.dump(2));

    ToolCall effective_call = call;

    // Resolve effective approval policy: per-workspace override wins over
    // the tool's built-in default.
    ToolApprovalPolicy policy = tool->approval_policy();
    if (auto* ws = services_.workspace()) {
        const auto& overrides = ws->config().tool_approval_policies;
        auto it = overrides.find(call.tool_name);
        if (it != overrides.end())
            policy = it->second;
    }

    if (policy == ToolApprovalPolicy::deny) {
        spdlog::info("ToolDispatcher: tool '{}' denied by workspace policy",
                     call.tool_name);
        ChatMessage denied;
        denied.role = MessageRole::tool;
        denied.tool_call_id = call.id;
        denied.content = "Tool call denied by workspace approval policy.";
        append_result(std::move(denied));
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_tool_result(call.id, "[denied by policy]");
        });
        return;
    }

    if (policy == ToolApprovalPolicy::auto_approve) {
        spdlog::trace("ToolDispatcher: auto-approve '{}'", call.tool_name);
    } else {
        std::string preview = tool->preview(call);
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_tool_call_pending(call, preview);
        });

        std::unique_lock lock(decision_mutex_);
        decision_cv_.wait(lock, [&] {
            return pending_decision_.has_value() || cancel_flag_.load();
        });

        if (cancel_flag_.load() && !pending_decision_.has_value()) {
            lock.unlock();
            ChatMessage cancelled;
            cancelled.role = MessageRole::tool;
            cancelled.tool_call_id = call.id;
            cancelled.content = "Tool call cancelled.";
            append_result(std::move(cancelled));
            frontends_.broadcast([&](IFrontend& fe) {
                fe.on_tool_result(call.id, "[cancelled]");
            });
            return;
        }

        ToolDecision decision = *pending_decision_;
        nlohmann::json mod_args = pending_modified_args_;
        pending_decision_.reset();
        pending_modified_args_ = {};
        lock.unlock();

        if (decision == ToolDecision::reject) {
            spdlog::info("ToolDispatcher: tool '{}' rejected by user",
                         call.tool_name);
            ChatMessage rejected;
            rejected.role = MessageRole::tool;
            rejected.tool_call_id = call.id;
            rejected.content = "Tool call rejected by user.";
            append_result(std::move(rejected));
            frontends_.broadcast([&](IFrontend& fe) {
                fe.on_tool_result(call.id, "[rejected]");
            });
            return;
        }

        if (decision == ToolDecision::modify && !mod_args.empty()) {
            effective_call.args = mod_args;
            spdlog::info("ToolDispatcher: tool '{}' args modified by user",
                         call.tool_name);
        }
    }

    // Snapshot the file *after* approval/modification but *before* the tool
    // mutates anything — so an Undo restores the bytes the user actually saw.
    maybe_snapshot(effective_call);

    auto result = tool->execute(effective_call, services_);

    spdlog::info("ToolDispatcher: tool '{}' result: success={}, content={} chars",
                 call.tool_name, result.success, result.content.size());
    spdlog::trace("ToolDispatcher: tool result content: {}", result.content);

    {
        std::string sum = "Tool result: " + call.tool_name;
        sum += result.success ? " (ok)" : " (failed)";
        sum += " — " + std::to_string(result.content.size()) + " chars";
        activity_.emit(result.success ? ActivityKind::tool_result
                                      : ActivityKind::error,
                       std::move(sum),
                       result.content);
    }

    ChatMessage tool_msg;
    tool_msg.role = MessageRole::tool;
    tool_msg.tool_call_id = call.id;
    tool_msg.content = result.content;
    append_result(std::move(tool_msg));

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_tool_result(call.id, result.display);
    });
}

} // namespace locus
