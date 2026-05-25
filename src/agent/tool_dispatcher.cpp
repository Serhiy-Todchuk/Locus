#include "tool_dispatcher.h"

#include "activity_log.h"
#include "checkpoint_store.h"
#include "file_change_tracker.h"
#include "metrics.h"
#include "tools/command_path_scanner.h"
#include "tools/shared.h"
#include "../core/log_channels.h"
#include "../core/workspace.h"

#include <spdlog/spdlog.h>

#include <exception>

#include <chrono>
#include <condition_variable>
#include <thread>
#include <utility>

namespace locus {

ToolDispatcher::ToolDispatcher(IToolRegistry& tools,
                               IWorkspaceServices& services,
                               ActivityLog& activity,
                               FrontendRegistry& frontends,
                               std::atomic<bool>& cancel_flag,
                               MetricsAggregator* metrics)
    : tools_(tools)
    , services_(services)
    , activity_(activity)
    , frontends_(frontends)
    , cancel_flag_(cancel_flag)
    , metrics_(metrics)
{}

void ToolDispatcher::submit_decision(const std::string& call_id,
                                     ToolDecision decision,
                                     nlohmann::json modified_args)
{
    std::lock_guard lock(decision_mutex_);
    if (awaiting_dispatches_.find(call_id) == awaiting_dispatches_.end()) {
        // Stale submission -- the dispatch for `call_id` already completed
        // (or was cancelled, or never reached the approval gate). Dropping
        // is safer than holding indefinitely; old click does nothing.
        spdlog::warn(
            "ToolDispatcher: dropping stale tool decision for call_id '{}'"
            " (no dispatch currently awaiting)", call_id);
        return;
    }
    pending_decisions_[call_id] =
        PendingDecision{decision, std::move(modified_args)};
    // notify_all -- multiple dispatches may be parked on this cv (future
    // parallel-tool-call work). Each waiter re-checks its own call.id.
    decision_cv_.notify_all();
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

void ToolDispatcher::set_change_tracker(FileChangeTracker* tracker)
{
    change_tracker_ = tracker;
}

void ToolDispatcher::set_runtime_preset(
    std::optional<tools::PermissionPreset> preset)
{
    if (preset && *preset == tools::PermissionPreset::custom) {
        spdlog::warn("ToolDispatcher: ignoring set_runtime_preset(custom) -- "
                     "the custom sentinel cannot be applied as a runtime override");
        return;
    }
    std::lock_guard lock(runtime_mutex_);
    runtime_preset_ = preset;
}

std::optional<tools::PermissionPreset> ToolDispatcher::runtime_preset() const
{
    std::lock_guard lock(runtime_mutex_);
    return runtime_preset_;
}

void ToolDispatcher::maybe_snapshot(const ToolCall& call)
{
    // Detect mutating tools once -- both the checkpoint snapshot (S4.B) and
    // the file-change tracker (S4.T) key off the same set.
    std::string action;
    if      (call.tool_name == "edit_file")   action = "edit";
    else if (call.tool_name == "write_file")  action = "write";
    else if (call.tool_name == "delete_file") action = "delete";
    else                                      return;

    std::string rel = call.args.value("path", "");
    if (rel.empty()) return;

    auto full = tools::resolve_path(services_, rel);
    if (full.empty()) return;  // outside workspace; tool will reject too

    if (checkpoints_ && !checkpoint_sid_.empty()) {
        checkpoints_->snapshot_before(checkpoint_sid_, checkpoint_turn_,
                                      services_.root(), full, action);
    }

    // S4.T -- record so this turn's writes don't echo back as "external" next
    // turn. Use the same workspace-relative form IndexQuery emits.
    if (change_tracker_) {
        std::error_code ec;
        auto rel_norm = std::filesystem::relative(full, services_.root(), ec);
        std::string key = ec ? rel : rel_norm.generic_string();
        change_tracker_->mark_agent_touched(key);
    }
}

void ToolDispatcher::dispatch(const ToolCall& call, const AppendFn& append_result)
{
    auto* tool = tools_.find(call.tool_name);

    if (!tool) {
        // S6.10 Task B -- enrich the dispatcher's unknown-tool error with a
        // closest-match suggestion + a short list of names the model could
        // have called, so the next round has actionable context instead of
        // just "your tool name is wrong". This replaces what the original
        // spec proposed as the `unknown_tool` quality-monitor detector.
        auto all = tools_.all();
        std::string suggestion = tools::closest_tool_name(call.tool_name, all);
        std::string available;
        for (std::size_t i = 0; i < all.size() && i < 24; ++i) {
            if (i) available += ", ";
            available += all[i]->name();
        }
        if (all.size() > 24) available += ", ...";

        spdlog::warn("ToolDispatcher: unknown tool '{}' (suggest '{}')",
                     call.tool_name, suggestion);
        ChatMessage err;
        err.role = MessageRole::tool;
        err.tool_call_id = call.id;
        err.content = "Error: unknown tool '" + call.tool_name + "'.";
        if (!suggestion.empty())
            err.content += " Did you mean '" + suggestion + "'?";
        err.content += " Available tools: " + available;
        append_result(std::move(err));
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_error("Unknown tool: " + call.tool_name);
        });
        activity_.emit(ActivityKind::error,
                       "Unknown tool: " + call.tool_name +
                           (suggestion.empty()
                                ? std::string{}
                                : " (suggest '" + suggestion + "')"),
                       "LLM requested tool '" + call.tool_name +
                           "' which is not registered.\n" +
                           (suggestion.empty()
                                ? std::string{}
                                : "Closest match: " + suggestion + "\n") +
                           "Available: " + available);
        return;
    }

    // S5.A -- the tool exists but its capability bucket is currently off.
    // The manifest didn't advertise it this turn, but the LLM may have
    // stale assumptions from a saved session or a prior turn. Surface a
    // one-line activity event that names the capability rather than the
    // generic "unknown tool" / lower-level execute error.
    if (!tool->available(services_)) {
        spdlog::warn("ToolDispatcher: tool '{}' disabled by capability gate",
                     call.tool_name);
        ChatMessage err;
        err.role = MessageRole::tool;
        err.tool_call_id = call.id;
        // The user-visible hint string lives in the activity event; the
        // LLM-visible payload stays short so it doesn't burn context.
        err.content = "Error: tool '" + call.tool_name +
                      "' is disabled in this workspace. Enable the "
                      "corresponding capability in Settings -> Capabilities.";
        append_result(std::move(err));

        // Which bucket the tool belongs to (for the activity event copy).
        std::string bucket = "the corresponding capability";
        if (call.tool_name == "run_command_bg"
            || call.tool_name == "read_process_output"
            || call.tool_name == "stop_process"
            || call.tool_name == "list_processes")
            bucket = "Background processes";
        else if (call.tool_name == "get_file_outline")
            bucket = "Code-aware search";
        else if (call.tool_name == "add_memory"
                 || call.tool_name == "search_memory")
            bucket = "Memory bank";

        activity_.emit(ActivityKind::warning,
                       "Tool '" + call.tool_name +
                           "' is disabled in this workspace",
                       bucket + " is off. Enable it in Settings -> "
                       "Capabilities to make the tool available.");
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_tool_result(call.id, "[disabled: " + bucket + "]", /*success=*/false);
        });
        return;
    }

    spdlog::info("ToolDispatcher: tool call '{}' id={}", call.tool_name, call.id);
    spdlog::trace("ToolDispatcher: tool args: {}",
                  truncate_for_log(call.args.dump()));

    activity_.emit(ActivityKind::tool_call,
                   "Tool call: " + call.tool_name,
                   "id: " + call.id + "\nargs: " + call.args.dump(2));

    ToolCall effective_call = call;

    // Resolve effective approval policy. Lookup order:
    //   1. S5.S runtime preset signature (when set).
    //   2. Exact / prefix override in WorkspaceConfig::tool_approval_policies.
    //   3. Tool's built-in default.
    // See tools::resolve_approval_policy for the workspace-config contract.
    ToolApprovalPolicy policy = tool->approval_policy();
    if (auto* ws = services_.workspace()) {
        policy = tools::resolve_approval_policy(
            call.tool_name, policy, ws->config().tool_approval_policies);
    }
    {
        std::optional<tools::PermissionPreset> rt;
        {
            std::lock_guard lock(runtime_mutex_);
            rt = runtime_preset_;
        }
        if (rt) {
            auto signature = tools::preset_signature(*rt, tools_);
            policy = tools::resolve_approval_policy(
                call.tool_name, policy, signature);
        }
    }

    // S4.V Task 5 -- outside-workspace path guard for shell tools. If the
    // command string references absolute paths, parent-traversal segments,
    // or env-var expansions, surface them as safety warnings and force the
    // approval gate open even when the workspace policy is auto_approve.
    // The check covers `run_command` and `run_command_bg` only; file tools
    // already refuse outside-workspace paths at the `resolve_path` layer.
    std::vector<std::string> safety_warnings;
    if (call.tool_name == "run_command" || call.tool_name == "run_command_bg") {
        std::string cmd = call.args.value("command", "");
        safety_warnings = tools::scan_outside_workspace_paths(
            cmd, services_.root());
        if (!safety_warnings.empty()
            && policy == ToolApprovalPolicy::auto_approve)
        {
            spdlog::warn("ToolDispatcher: '{}' references {} outside-workspace "
                         "path token(s); upgrading auto_approve -> ask",
                         call.tool_name, safety_warnings.size());
            policy = ToolApprovalPolicy::ask;
        }
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
            fe.on_tool_result(call.id, "[denied by policy]", /*success=*/false);
        });
        return;
    }

    // Register the call as awaiting a decision BEFORE broadcasting
    // on_tool_call_pending. The harness frontend (and any frontend that
    // auto-approves synchronously inside the callback) calls submit_decision
    // before returning from on_tool_call_pending. submit_decision checks
    // awaiting_dispatches_ and drops the decision if the call_id is absent --
    // so the insert must precede the broadcast to avoid the race.
    // For auto-approve calls we still insert+erase so the book-keeping is
    // symmetric; the condvar path is skipped entirely below.
    if (policy != ToolApprovalPolicy::auto_approve) {
        std::lock_guard pre_lock(decision_mutex_);
        awaiting_dispatches_.insert(call.id);
    }

    // Always notify frontends that a tool call is in flight, regardless of
    // policy. The chat panel needs this to render the tool-name header above
    // the eventual result; without it, results land in the chat with no
    // identifying header (and visually attach to whatever DOM element the
    // chat panel last rendered, which is usually the reasoning <details>
    // block). The approval panel (GUI) checks the policy itself and skips
    // popping for auto-approved calls -- see locus_frame.cpp.
    {
        std::string preview = tool->preview(call);
        bool needs_approval = (policy != ToolApprovalPolicy::auto_approve);
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_tool_call_pending(call, preview, needs_approval,
                                    safety_warnings);
        });
    }

    if (policy == ToolApprovalPolicy::auto_approve) {
        spdlog::trace("ToolDispatcher: auto-approve '{}'", call.tool_name);
    } else {
        // Approval gate: awaiting_dispatches_ was already populated above.
        // Just wait for the decision (or cancellation).

        std::unique_lock lock(decision_mutex_);
        // awaiting_dispatches_ already has call.id -- no second insert needed.
        decision_cv_.wait(lock, [&] {
            return pending_decisions_.find(call.id) != pending_decisions_.end()
                   || cancel_flag_.load();
        });

        auto it = pending_decisions_.find(call.id);
        if (cancel_flag_.load() && it == pending_decisions_.end()) {
            awaiting_dispatches_.erase(call.id);
            lock.unlock();
            ChatMessage cancelled;
            cancelled.role = MessageRole::tool;
            cancelled.tool_call_id = call.id;
            cancelled.content = "Tool call cancelled.";
            append_result(std::move(cancelled));
            frontends_.broadcast([&](IFrontend& fe) {
                fe.on_tool_result(call.id, "[cancelled]", /*success=*/false);
            });
            return;
        }

        ToolDecision   decision = it->second.decision;
        nlohmann::json mod_args = std::move(it->second.modified_args);
        pending_decisions_.erase(it);
        awaiting_dispatches_.erase(call.id);
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
                fe.on_tool_result(call.id, "[rejected]", /*success=*/false);
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
    // mutates anything -- so an Undo restores the bytes the user actually saw.
    maybe_snapshot(effective_call);

    auto exec_t0 = std::chrono::steady_clock::now();
    // S5.Z task 7 -- thread the agent's cancel flag through so long-running
    // tools (run_command, MCP) can abort in flight rather than waiting for
    // the natural timeout.
    //
    // S5.Z follow-up -- wall-clock guardrail. When WorkspaceConfig::Agent::tool_max_runtime_s > 0,
    // arm a watchdog thread that sets the cancel_flag if the tool hasn't returned
    // by then. Saves a session from the run_command ReadFile hang documented in
    // tests/ui_automation/output/agentic_Tetris/findings.md when the tool's own
    // timeout path is broken. Cleared via the watchdog_stop cv after execute returns.
    int max_runtime_s = 0;
    if (auto* ws = services_.workspace())
        max_runtime_s = ws->config().agent.tool_max_runtime_s;

    std::atomic<bool> watchdog_fired{false};
    std::mutex                wd_mu;
    std::condition_variable   wd_cv;
    bool                      wd_stop = false;
    std::thread               watchdog;
    if (max_runtime_s > 0) {
        watchdog = std::thread([&, tool_name = call.tool_name]{
            std::unique_lock<std::mutex> lk(wd_mu);
            if (wd_cv.wait_for(lk, std::chrono::seconds(max_runtime_s),
                               [&]{ return wd_stop; })) {
                return;  // tool returned cleanly
            }
            // Timed out: trip the cancel_flag and shout in the log. The
            // tool's own polling loop (process_tools.cpp etc.) sees the flag
            // and unwinds; the result we synthesize below carries a clear
            // "timeout" message so the LLM doesn't confuse this with a
            // user cancel.
            watchdog_fired.store(true, std::memory_order_release);
            cancel_flag_.store(true, std::memory_order_release);
            spdlog::warn(
                "ToolDispatcher: tool '{}' wall-clock guardrail fired ({}s); "
                "tripping cancel_flag",
                tool_name, max_runtime_s);
        });
    }

    // Defence-in-depth: any unhandled exception from a tool's execute()
    // would otherwise unwind through the agent thread's run loop, hit
    // std::terminate, and crash the whole process (the GUI included).
    // Surface it to the LLM as a clean tool-result error so the agent can
    // self-correct on the next round and the user keeps their session.
    // See tests/ui_automation/output/agentic_Tetris/findings.md #8 for the
    // observed locus_gui exit after a malformed `edit_file` payload.
    ToolResult result;
    try {
        result = tool->execute(effective_call, services_, &cancel_flag_);
    } catch (const std::exception& ex) {
        spdlog::error("ToolDispatcher: tool '{}' threw: {}",
                      call.tool_name, ex.what());
        result.success = false;
        result.content = std::string("[tool '") + call.tool_name +
                         "' failed with an internal error: " + ex.what() +
                         ". Try a different argument shape or a different tool.]";
        result.display = result.content;
    } catch (...) {
        spdlog::error("ToolDispatcher: tool '{}' threw an unknown exception",
                      call.tool_name);
        result.success = false;
        result.content = std::string("[tool '") + call.tool_name +
                         "' failed with an unknown internal error. "
                         "Try a different argument shape or a different tool.]";
        result.display = result.content;
    }

    if (watchdog.joinable()) {
        {
            std::lock_guard<std::mutex> lk(wd_mu);
            wd_stop = true;
        }
        wd_cv.notify_all();
        watchdog.join();
    }

    // If the guardrail fired, rewrite the result so the LLM sees a clean
    // timeout message rather than whatever partial output the tool returned
    // after its cancel-flag unwound. Also clear cancel_flag_ -- it was set
    // by our watchdog, not by the user; leaving it set would short-circuit
    // the next tool dispatch too.
    if (watchdog_fired.load(std::memory_order_acquire)) {
        cancel_flag_.store(false, std::memory_order_release);
        result.success = false;
        result.content = "[tool wall-clock guardrail fired after " +
                         std::to_string(max_runtime_s) +
                         "s without return; the call was cancelled. "
                         "If this is a legitimate long-running build, "
                         "raise agent.tool_max_runtime_s in .locus/config.json "
                         "or set it to 0 to disable the guardrail.]";
    }

    auto exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - exec_t0).count();

    spdlog::info("ToolDispatcher: tool '{}' result: success={}, content={} chars, exec_ms={}",
                 call.tool_name, result.success, result.content.size(), (long long)exec_ms);
    spdlog::trace("ToolDispatcher: tool result content: {}", result.content);

    if (metrics_) {
        std::optional<int> rcount = parse_search_result_count(call.tool_name,
                                                              result.content);
        std::optional<int> rcap;
        if (rcount.has_value()) {
            // search_text/symbols default to 20, semantic/hybrid 10, regex 50.
            int default_cap = 20;
            if (call.tool_name == "search_semantic"
                || call.tool_name == "search_hybrid")
                default_cap = 10;
            else if (call.tool_name == "search_regex")
                default_cap = 50;
            rcap = effective_call.args.value("max_results", default_cap);
        }
        metrics_->record_tool(call.tool_name, result.success, exec_ms,
                              result.content.size(), rcount, rcap);
    }

    {
        std::string sum = "Tool result: " + call.tool_name;
        sum += result.success ? " (ok)" : " (failed)";
        sum += " -- " + std::to_string(result.content.size()) + " chars";
        activity_.emit(result.success ? ActivityKind::tool_result
                                      : ActivityKind::error,
                       std::move(sum),
                       result.content);
    }

    // S6.10 Task G -- optional companion event when the tool flagged its
    // result with an activity tag (today: truncation_blocked from file tools).
    if (!result.activity_tag.empty()) {
        ActivityKind kind = ActivityKind::warning;
        if (result.activity_tag == "truncation_blocked")
            kind = ActivityKind::truncation_blocked;
        else if (result.activity_tag == "quality_correction")
            kind = ActivityKind::quality_correction;
        activity_.emit(kind,
                       result.activity_summary.empty()
                           ? std::string{"Tool flagged: "} + result.activity_tag
                           : result.activity_summary,
                       result.activity_detail);
    }

    ChatMessage tool_msg;
    tool_msg.role = MessageRole::tool;
    tool_msg.tool_call_id = call.id;
    tool_msg.content = result.content;
    append_result(std::move(tool_msg));

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_tool_result(call.id, result.display, result.success);
    });
}

} // namespace locus
