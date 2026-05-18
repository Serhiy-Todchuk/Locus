#include "agent_core.h"

#include "auto_commit.h"
#include "compaction_pipeline.h"
#include "history_archive.h"
#include "index/index_query.h"
#include "llm/token_counter.h"
#include "../tools/tool_registry.h"
#include "../core/memory_store.h"
#include "../core/workspace.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace locus {

namespace {

// S5.F -- snapshot the workspace compaction config into a per-run selection.
// AgentCore uses this from three places (legacy strategy shim, auto-compact
// trigger in the round loop, /compact slash command) so it lives at file
// scope.
CompactionLayerSelection selection_from_config(const WorkspaceConfig::Compaction& c)
{
    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results           = c.layer_drop_redundant_tool_results;
    sel.strip_large_tool_bodies               = c.layer_strip_large_tool_bodies;
    sel.drop_old_reasoning                    = c.layer_drop_old_reasoning;
    sel.drop_oldest_turns                     = c.layer_drop_oldest_turns;
    sel.llm_summary                           = c.layer_llm_summary;
    sel.strip_threshold_tokens                = c.strip_threshold_tokens;
    sel.older_than_turns                      = c.older_than_turns;
    sel.keep_recent_turns                     = c.keep_recent_turns;
    sel.preserve_short_user_msgs_max_tokens   = c.preserve_short_user_msgs_max_tokens;
    sel.preserve_short_tool_calls_max_tokens  = c.preserve_short_tool_calls_max_tokens;
    sel.summary_max_tokens                    = c.summary_max_tokens;
    sel.custom_summary_instructions           = c.custom_summary_instructions;
    return sel;
}

} // namespace

AgentCore::AgentCore(ILLMClient& llm,
                     IToolRegistry& tools,
                     IWorkspaceServices& services,
                     const std::string& locus_md,
                     const WorkspaceMetadata& ws_meta,
                     const LLMConfig& llm_config,
                     const std::filesystem::path& sessions_dir,
                     const std::filesystem::path& checkpoints_dir,
                     const std::filesystem::path& project_prompts_dir,
                     const std::filesystem::path& global_prompts_dir)
    : llm_(llm)
    , tools_(tools)
    , services_(services)
    , sessions_(sessions_dir)
{
    // S4.R -- pre-render the always-in-context memory slot before the prompt
    // is assembled. Captured at session start so the resulting prompt stays
    // byte-stable across the session (S4.F KV-cache invariant).
    std::string memory_section;
    if (auto* mem = services.memory()) {
        try { memory_section = mem->format_for_system_prompt(); }
        catch (const std::exception& e) {
            spdlog::warn("MemoryStore: format_for_system_prompt failed: {}", e.what());
        }
    }

    // S5.J -- system prompt is an immutable SystemPromptAssembly value. Build
    // once here and hand to LLMContext. There is no setter; a new prompt = a
    // new assembly (and a new LLMContext) by design.
    auto assembly = SystemPromptAssembly::build(
        locus_md, ws_meta, tools, llm_config.tool_format, memory_section);

    int sys_tokens = assembly.total_tokens();
    spdlog::info("AgentCore: system prompt ~{} tokens, context limit {}",
                 sys_tokens, llm_config.context_limit);

    activity_   = std::make_unique<ActivityLog>(frontends_);
    metrics_    = std::make_unique<MetricsAggregator>();

    // LLMContext owns history + budget + tracker + checkpoints + attached.
    // Constructor seeds the system message and the initial mtime snapshot.
    ctx_ = std::make_unique<LLMContext>(services_, llm_config,
                                         std::move(assembly),
                                         frontends_, sessions_,
                                         checkpoints_dir);

    loop_ = std::make_unique<AgentLoop>(llm_, tools_, services_,
                                         *activity_, ctx_->budget(), frontends_,
                                         cancel_requested_,
                                         metrics_.get());
    dispatcher_ = std::make_unique<ToolDispatcher>(tools_, services_, *activity_,
                                                   frontends_, cancel_requested_,
                                                   metrics_.get());
    slash_      = std::make_unique<SlashCommandDispatcher>(tools_, services_);

    // S4.X -- prompt templates.
    if (!project_prompts_dir.empty() || !global_prompts_dir.empty()) {
        prompt_templates_ = std::make_unique<PromptTemplateRegistry>(
            project_prompts_dir, global_prompts_dir);
        slash_->set_template_registry(prompt_templates_.get());
    }

    // S4.T -- wire the file-change tracker into the dispatcher so mutating
    // tool calls record their paths (suppressing them from next turn's
    // "files changed since last turn" notification).
    dispatcher_->set_change_tracker(ctx_->change_tracker());

    // S5.F -- pre-compaction history archive lives under the same sessions
    // directory the SessionManager uses, but inside a per-session sub-folder
    // so the live `<id>.json` and the chained archives stay distinct.
    history_archive_ = std::make_unique<HistoryArchive>(sessions_dir);

    activity_->emit(ActivityKind::system_prompt,
                    "System prompt assembled (~" + std::to_string(sys_tokens) + " tokens)",
                    ctx_->system_prompt().full_text(),
                    /*tokens_in=*/std::nullopt,
                    /*tokens_out=*/std::nullopt,
                    /*tokens_delta=*/sys_tokens);
}

AgentCore::~AgentCore()
{
    stop();
}

// -- Activity catch-up / external emit ---------------------------------------

std::vector<ActivityEvent> AgentCore::get_activity(uint64_t since_id) const
{
    return activity_->get_since(since_id);
}

void AgentCore::emit_index_event(const std::string& summary, const std::string& detail)
{
    activity_->emit_index_event(summary, detail);
}

void AgentCore::delete_message(int history_id)
{
    if (history_id <= 0) {
        spdlog::warn("AgentCore::delete_message: ignoring non-positive id {}", history_id);
        return;
    }
    {
        std::lock_guard lock(queue_mutex_);
        pending_delete_ids_.push_back(history_id);
    }
    queue_cv_.notify_one();
    spdlog::info("AgentCore: queued delete for history_id={}", history_id);
}

void AgentCore::apply_pending_deletes()
{
    std::vector<int> ids;
    {
        std::lock_guard lock(queue_mutex_);
        ids.swap(pending_delete_ids_);
    }
    if (ids.empty()) return;
    for (int id : ids) {
        bool ok = ctx_->delete_message(id);
        if (ok) {
            activity_->emit(ActivityKind::index_event,
                            "Deleted history message",
                            "history_id=" + std::to_string(id));
        } else {
            spdlog::warn("AgentCore: delete_message({}) -- not found or refused", id);
        }
    }
    // Update context-meter so the UI gauge reflects the freed tokens.
    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(ctx_->current_tokens(), ctx_->llm_config().context_limit,
                            ctx_->budget().last_prompt_tokens(),
                            ctx_->budget().last_completion_tokens(),
                            ctx_->budget().reserve());
    });
}

// -- Attached file context (S2.4 / S4.F) -------------------------------------

void AgentCore::set_attached_context(AttachedContext ctx)
{
    ctx_->set_attached_context(ctx);

    spdlog::info("AgentCore: attached context set to '{}'", ctx.file_path);
    activity_->emit(ActivityKind::index_event,
                    "Attached file: " + ctx.file_path,
                    "Outline injected on next user turn (system prompt stays "
                    "stable for KV-cache reuse -- S4.F).");

    auto snapshot = std::optional<AttachedContext>{ctx};
    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_attached_context_changed(snapshot);
    });
}

void AgentCore::clear_attached_context()
{
    bool had_value = ctx_->attached_context().has_value();
    ctx_->clear_attached_context();
    if (!had_value) return;

    spdlog::info("AgentCore: attached context cleared");
    activity_->emit(ActivityKind::index_event, "Detached file from context", "");

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_attached_context_changed(std::nullopt);
    });
}

std::optional<AttachedContext> AgentCore::attached_context() const
{
    return ctx_->attached_context();
}

// -- Plan mode (S4.D) --------------------------------------------------------

AgentMode AgentCore::mode() const
{
    return mode_.load(std::memory_order_acquire);
}

void AgentCore::set_mode(AgentMode m)
{
    pending_mode_.store(m, std::memory_order_relaxed);
    pending_mode_change_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(queue_mutex_);
    }
    queue_cv_.notify_one();
    spdlog::info("AgentCore: mode change queued: {}", to_string(m));
}

void AgentCore::approve_plan()
{
    pending_plan_approve_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(queue_mutex_);
    }
    queue_cv_.notify_one();
    spdlog::info("AgentCore: plan approval queued");
}

void AgentCore::reject_plan()
{
    pending_plan_reject_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(queue_mutex_);
    }
    queue_cv_.notify_one();
    spdlog::info("AgentCore: plan rejection queued");
}

std::optional<Plan> AgentCore::current_plan() const
{
    std::lock_guard lock(plan_mutex_);
    return current_plan_;
}

void AgentCore::apply_pending_mode_change()
{
    if (!pending_mode_change_.exchange(false, std::memory_order_acq_rel))
        return;

    AgentMode m = pending_mode_.load(std::memory_order_relaxed);
    AgentMode prev = mode_.exchange(m, std::memory_order_acq_rel);
    if (m == prev) return;

    if (m == AgentMode::chat || (m == AgentMode::plan && prev != AgentMode::plan)) {
        std::lock_guard lock(plan_mutex_);
        current_plan_.reset();
        plan_awaiting_decision_ = false;
    }

    spdlog::info("AgentCore: mode {} -> {}", to_string(prev), to_string(m));
    activity_->emit(ActivityKind::index_event,
                    std::string("Mode: ") + to_string(m),
                    std::string("Switched from ") + to_string(prev) +
                        " to " + to_string(m));
    frontends_.broadcast([&](IFrontend& fe) { fe.on_mode_changed(m); });
}

void AgentCore::apply_pending_plan_decision()
{
    bool approve = pending_plan_approve_.exchange(false, std::memory_order_acq_rel);
    bool reject  = pending_plan_reject_.exchange(false, std::memory_order_acq_rel);
    if (!approve && !reject) return;

    Plan plan;
    bool have_plan = false;
    bool awaiting  = false;
    {
        std::lock_guard lock(plan_mutex_);
        if (current_plan_.has_value()) {
            plan = *current_plan_;
            have_plan = true;
        }
        awaiting = plan_awaiting_decision_;
    }

    if (!have_plan || !awaiting) {
        spdlog::warn("AgentCore: plan decision queued but no plan awaiting");
        return;
    }

    if (reject) {
        {
            std::lock_guard lock(plan_mutex_);
            current_plan_.reset();
            plan_awaiting_decision_ = false;
        }
        spdlog::info("AgentCore: plan '{}' rejected", plan.id);
        activity_->emit(ActivityKind::index_event,
                        "Plan rejected", plan.title);
        return;
    }

    AgentMode prev_mode = mode_.exchange(AgentMode::execute,
                                          std::memory_order_acq_rel);
    {
        std::lock_guard lock(plan_mutex_);
        plan_awaiting_decision_ = false;
    }

    std::string recap = "[Plan approved -- executing]\n" +
                        std::string("Title: ") + plan.title + "\n";
    if (!plan.summary.empty())
        recap += "Summary: " + plan.summary + "\n";
    recap += "Steps:\n";
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        recap += "  " + std::to_string(i + 1) + ". " + plan.steps[i].description;
        if (!plan.steps[i].tools_needed.empty()) {
            recap += " (tools: ";
            for (size_t k = 0; k < plan.steps[i].tools_needed.size(); ++k) {
                if (k) recap += ", ";
                recap += plan.steps[i].tools_needed[k];
            }
            recap += ")";
        }
        recap += "\n";
    }
    recap += "Use mark_step_done as you complete each step. "
             "Begin with step 1.";

    spdlog::info("AgentCore: plan '{}' approved (mode {} -> execute)",
                 plan.id, to_string(prev_mode));
    activity_->emit(ActivityKind::index_event,
                    "Plan approved", plan.title);

    if (prev_mode != AgentMode::execute) {
        frontends_.broadcast([](IFrontend& fe) {
            fe.on_mode_changed(AgentMode::execute);
        });
    }

    {
        std::lock_guard lock(queue_mutex_);
        message_queue_.push(std::move(recap));
    }
    queue_cv_.notify_one();
}

// -- Start / Stop ------------------------------------------------------------

void AgentCore::start()
{
    if (running_.load())
        return;

    running_.store(true);
    agent_thread_ = std::thread(&AgentCore::agent_thread_func, this);
    spdlog::info("AgentCore: agent thread started");
}

void AgentCore::stop()
{
    if (!running_.load())
        return;

    running_.store(false);
    cancel_requested_.store(true);

    queue_cv_.notify_one();
    dispatcher_->wake();

    if (agent_thread_.joinable())
        agent_thread_.join();

    spdlog::info("AgentCore: agent thread stopped");
}

// -- Agent thread ------------------------------------------------------------

void AgentCore::agent_thread_func()
{
    spdlog::trace("AgentCore: agent thread running");

    while (running_.load()) {
        std::string message;
        bool have_message = false;

        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [&] {
                return !message_queue_.empty()
                       || pending_compact_.load(std::memory_order_acquire)
                       || pending_pipeline_
                       || pending_mode_change_.load(std::memory_order_acquire)
                       || pending_plan_approve_.load(std::memory_order_acquire)
                       || pending_plan_reject_.load(std::memory_order_acquire)
                       || !pending_delete_ids_.empty()
                       || !running_.load();
            });

            if (!running_.load())
                break;

            if (!message_queue_.empty()) {
                message = std::move(message_queue_.front());
                message_queue_.pop();
                have_message = true;
            }
        }

        // Drain pending control requests before the turn starts. Each takes
        // the conversation owner scope while it runs.
        {
            ConversationOwnerScope owner_scope(ctx_->history());
            apply_pending_compaction();
            apply_pending_mode_change();
            apply_pending_plan_decision();
            apply_pending_deletes();
        }

        if (!have_message)
            continue;

        {
            ConversationOwnerScope owner_scope(ctx_->history());
            process_message(message);
        }

        {
            std::lock_guard lock(sync_mutex_);
            sync_turn_done_ = true;
        }
        sync_cv_.notify_all();
    }

    spdlog::trace("AgentCore: agent thread exiting");
}

// -- Frontend registration ----------------------------------------------------

void AgentCore::register_frontend(IFrontend* fe)
{
    frontends_.register_frontend(fe);
    // Late-attaching frontend: send the current effective preset so its chip
    // renders in the right state without waiting for the next change.
    if (fe) {
        tools::PermissionPreset eff = effective_permission_preset();
        bool from_runtime = dispatcher_ && dispatcher_->runtime_preset().has_value();
        fe->on_permission_preset_changed(eff, from_runtime);
    }
}

void AgentCore::unregister_frontend(IFrontend* fe)
{
    frontends_.unregister_frontend(fe);
}

// -- S5.S Permission preset runtime override ---------------------------------

tools::PermissionPreset AgentCore::effective_permission_preset() const
{
    if (dispatcher_) {
        if (auto rt = dispatcher_->runtime_preset())
            return *rt;
    }
    if (auto* ws = services_.workspace()) {
        return tools::detect_preset(ws->config().tool_approval_policies, tools_);
    }
    return tools::PermissionPreset::ask_before_edits;
}

void AgentCore::set_runtime_permission_preset(tools::PermissionPreset preset)
{
    if (preset == tools::PermissionPreset::custom) {
        spdlog::warn("AgentCore: set_runtime_permission_preset(custom) ignored");
        return;
    }
    if (!dispatcher_) return;
    auto prev = dispatcher_->runtime_preset();
    dispatcher_->set_runtime_preset(preset);

    std::string detail = std::string("Runtime override -> ")
                       + tools::display_name(preset);
    if (prev) detail = std::string("Runtime override: ")
                     + tools::display_name(*prev) + " -> "
                     + tools::display_name(preset);

    activity_->emit(ActivityKind::warning,
                    std::string("Permission preset: ")
                        + tools::display_name(preset)
                        + " (runtime override)",
                    detail);
    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_permission_preset_changed(preset, /*from_runtime=*/true);
    });
}

void AgentCore::clear_runtime_permission_preset()
{
    if (!dispatcher_) return;
    if (!dispatcher_->runtime_preset().has_value()) return;

    dispatcher_->set_runtime_preset(std::nullopt);
    tools::PermissionPreset eff = effective_permission_preset();
    activity_->emit(ActivityKind::warning,
                    "Permission preset: runtime override cleared",
                    std::string("Falling back to workspace setting: ")
                        + tools::display_name(eff));
    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_permission_preset_changed(eff, /*from_runtime=*/false);
    });
}

std::optional<tools::PermissionPreset>
AgentCore::runtime_permission_preset() const
{
    if (!dispatcher_) return std::nullopt;
    return dispatcher_->runtime_preset();
}

void AgentCore::rebroadcast_permission_preset(bool from_runtime)
{
    tools::PermissionPreset eff = effective_permission_preset();
    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_permission_preset_changed(eff, from_runtime);
    });
}

// -- send_message (non-blocking) ---------------------------------------------

void AgentCore::send_message(const std::string& content)
{
    spdlog::info("AgentCore: queuing user message ({} chars)", content.size());

    {
        std::lock_guard lock(queue_mutex_);
        message_queue_.push(content);
    }
    queue_cv_.notify_one();
}

// -- send_message_sync (blocking, for CLI) -----------------------------------

void AgentCore::send_message_sync(const std::string& content)
{
    {
        std::lock_guard lock(sync_mutex_);
        sync_turn_done_ = false;
    }

    send_message(content);

    std::unique_lock lock(sync_mutex_);
    sync_cv_.wait(lock, [&] { return sync_turn_done_; });
}

// -- process_message (runs on agent thread) ----------------------------------

void AgentCore::process_message(const std::string& content)
{
    spdlog::info("AgentCore: processing user message ({} chars)", content.size());

    busy_.store(true);
    cancel_requested_.store(false);

    frontends_.broadcast([](IFrontend& fe) { fe.on_turn_start(); });

    std::string user_input = content;
    {
        auto outcome = try_slash_command(content);
        if (outcome.kind == SlashKind::handled) {
            frontends_.broadcast([](IFrontend& fe) { fe.on_turn_complete(); });
            busy_.store(false);
            return;
        }
        if (outcome.kind == SlashKind::expanded) {
            user_input = std::move(outcome.expanded_text);
            spdlog::info("AgentCore: prompt template expanded -> {} chars",
                         user_input.size());
        }
    }

    // Bump turn counter + arm checkpoint snapshotting. Slash commands above
    // don't bump -- they bypass the dispatcher.
    int turn_id = ctx_->bump_turn_id();
    if (auto* cps = ctx_->checkpoints()) {
        dispatcher_->set_turn_context(cps, ctx_->session_id(), turn_id);
        cps->gc_session(ctx_->session_id());
    }
    metrics_->begin_turn(turn_id);

    // S4.T / S4.F volatile-tail prepends.
    std::string effective_content = user_input;
    {
        std::string attached_block = ctx_->compose_attached_context_block();
        if (!attached_block.empty())
            effective_content = attached_block + effective_content;
    }
    bool notify = true;
    if (auto* ws = services_.workspace())
        notify = ws->config().notify_external_changes;
    if (notify && ctx_->change_tracker()) {
        if (auto* idx = services_.index()) {
            try {
                auto diff = ctx_->change_tracker()->diff_since_snapshot(*idx);
                if (!diff.changed.empty() || !diff.deleted.empty()) {
                    std::ostringstream note;
                    note << "[Files changed since last turn:";
                    bool first = true;
                    for (const auto& p : diff.changed) {
                        note << (first ? " " : ", ") << p;
                        first = false;
                    }
                    for (const auto& p : diff.deleted) {
                        note << (first ? " " : ", ") << p << " (deleted)";
                        first = false;
                    }
                    note << "]\n\n";
                    effective_content = note.str() + effective_content;
                    spdlog::info("AgentCore: external changes since last turn: "
                                 "{} modified, {} deleted",
                                 diff.changed.size(), diff.deleted.size());
                    activity_->emit(ActivityKind::index_event,
                                    "External file changes detected",
                                    note.str());
                }
            } catch (const std::exception& e) {
                spdlog::warn("FileChangeTracker: diff failed: {}", e.what());
            }
        }
        ctx_->change_tracker()->clear_agent_touched();
    }

    ctx_->add_message({MessageRole::user, effective_content});

    // S5.D -- pass the per-message token estimate as tokens_in so the
    // activity log can show how much each turn costs.
    int user_msg_tokens = ctx_->history().messages().back().token_estimate;
    activity_->emit(ActivityKind::user_message,
                    "User message (" + std::to_string(user_input.size()) + " chars)",
                    user_input,
                    /*tokens_in=*/user_msg_tokens);

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(ctx_->current_tokens(), ctx_->llm_config().context_limit,
                            ctx_->budget().last_prompt_tokens(),
                            ctx_->budget().last_completion_tokens(),
                            ctx_->budget().reserve());
    });

    int round = 0;
    bool turn_had_error = false;
    static constexpr int k_max_rounds = 20;

    while (round < k_max_rounds) {
        if (cancel_requested_.load()) {
            spdlog::info("AgentCore: turn cancelled by user");
            frontends_.broadcast([](IFrontend& fe) {
                fe.on_error("Turn cancelled.");
            });
            break;
        }

        ++round;
        spdlog::trace("AgentCore: LLM round {}", round);

        apply_pending_compaction();
        apply_pending_mode_change();
        apply_pending_plan_decision();
        apply_pending_deletes();

        // S5.F -- auto-compact band. When usage crosses the configured
        // auto_threshold of effective_limit and auto_enabled is on, queue
        // the cascade (1+2+3+6, layer 5 reserved for explicit user invocation)
        // and apply it immediately so the next round runs with a leaner history.
        if (auto* ws = services_.workspace()) {
            const auto& cc = ws->config().compaction;
            // Only consider the auto band when the LLM context window is
            // actually known (auto-detected > 0). Otherwise effective_limit
            // collapses to 1 and a 2-K system prompt looks like 200000% usage.
            int  raw_limit = ctx_->llm_config().context_limit;
            if (cc.auto_enabled && raw_limit > 0) {
                int eff  = ctx_->effective_limit();
                int used = ctx_->current_tokens();
                if (eff > 0 && used > 0) {
                    double ratio = static_cast<double>(used) / eff;
                    if (ratio >= cc.auto_threshold) {
                        CompactionLayerSelection sel = selection_from_config(cc);
                        int target = static_cast<int>(eff * cc.warn_threshold);
                        if (target <= 0) target = eff / 2;
                        spdlog::warn("AgentCore: auto-compact firing ({} / {} = {:.0f}% of effective_limit, threshold {:.0f}%)",
                                     used, eff, ratio * 100, cc.auto_threshold * 100);
                        {
                            std::lock_guard lock(queue_mutex_);
                            pending_pipeline_           = true;
                            pending_pipeline_selection_ = sel;
                            pending_pipeline_target_    = target;
                            pending_pipeline_overrides_.clear();
                        }
                        apply_pending_compaction();
                    }
                }
            }
        }

        if (ctx_->budget().check_overflow(ctx_->current_tokens()))
            break;

        ToolMode tool_mode = ToolMode::agent;
        switch (mode_.load(std::memory_order_acquire)) {
        case AgentMode::chat:    tool_mode = ToolMode::agent;   break;
        case AgentMode::plan:    tool_mode = ToolMode::plan;    break;
        case AgentMode::execute: tool_mode = ToolMode::execute; break;
        }
        auto step = loop_->run_step(ctx_->history(), tool_mode);

        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_context_meter(ctx_->current_tokens(), ctx_->llm_config().context_limit,
                            ctx_->budget().last_prompt_tokens(),
                            ctx_->budget().last_completion_tokens(),
                            ctx_->budget().reserve());
        });

        if (step.had_error) {
            turn_had_error = true;
            break;
        }

        if (cancel_requested_.load()) {
            ctx_->add_message(std::move(step.assistant_msg));
            spdlog::info("AgentCore: turn cancelled mid-stream");
            frontends_.broadcast([](IFrontend& fe) {
                fe.on_error("Turn cancelled.");
            });
            break;
        }

        ctx_->add_message(std::move(step.assistant_msg));

        if (step.tool_calls.empty())
            break;

        for (auto& call : step.tool_calls) {
            if (cancel_requested_.load()) {
                spdlog::info("AgentCore: skipping remaining tool calls (cancelled)");
                break;
            }
            std::string captured_content;
            bool        captured_success = true;
            dispatcher_->dispatch(call, [&](ChatMessage m) {
                captured_content = m.content;
                ctx_->add_message(std::move(m));
            });
            observe_plan_tool_result(call, captured_content, captured_success);
        }

        {
            std::lock_guard lock(plan_mutex_);
            if (plan_awaiting_decision_)
                break;
        }
    }

    if (round >= k_max_rounds) {
        turn_had_error = true;
        spdlog::warn("AgentCore: hit max round limit ({})", k_max_rounds);
        frontends_.broadcast([](IFrontend& fe) {
            fe.on_error("Agent reached the maximum number of tool call rounds.");
        });
    }

    metrics_->end_turn(turn_had_error);

    // Drop the turn context so any background activity past this point won't
    // accidentally tag a snapshot to the just-finished turn.
    if (auto* cps = ctx_->checkpoints()) {
        if (cps->entry_count(ctx_->session_id(), turn_id) == 0)
            cps->drop_turn(ctx_->session_id(), turn_id);
        dispatcher_->set_turn_context(nullptr, {}, 0);
    }

    // S4.L -- per-turn auto-commit.
    if (!turn_had_error
        && ctx_->change_tracker()
        && ctx_->change_tracker()->agent_touched_size() > 0)
    {
        if (auto* ws = services_.workspace()) {
            const auto& cfg = ws->config();
            if (cfg.git_auto_commit) {
                std::string assistant_text;
                for (auto it = ctx_->history().messages().rbegin();
                     it != ctx_->history().messages().rend(); ++it) {
                    if (it->role == MessageRole::assistant && !it->content.empty()) {
                        assistant_text = it->content;
                        break;
                    }
                }
                auto r = auto_commit_after_turn(
                    services_.root(),
                    cfg.git_commit_prefix,
                    cfg.git_commit_branch,
                    assistant_text);

                if (r.success && !r.skipped) {
                    activity_->emit(ActivityKind::index_event,
                                    "Auto-commit " + r.short_sha
                                        + " on " + r.branch,
                                    cfg.git_commit_prefix
                                        + make_commit_subject(assistant_text));
                    std::string sha = r.short_sha;
                    std::string br  = r.branch;
                    std::string sub = make_commit_subject(assistant_text);
                    frontends_.broadcast([&](IFrontend& fe) {
                        fe.on_auto_commit(sha, br, sub);
                    });
                } else if (!r.success) {
                    spdlog::warn("auto_commit failed: {}", r.error);
                    activity_->emit(ActivityKind::error,
                                    "Auto-commit failed",
                                    r.error);
                }
            }
        }
    }

    // S4.T -- refresh the mtime baseline for next turn's diff.
    ctx_->snapshot_change_tracker();

    frontends_.broadcast([](IFrontend& fe) { fe.on_turn_complete(); });
    busy_.store(false);
}

// -- is_busy / cancel_turn ---------------------------------------------------

bool AgentCore::is_busy() const
{
    return busy_.load();
}

void AgentCore::cancel_turn()
{
    if (busy_.load()) {
        cancel_requested_.store(true);
        dispatcher_->wake();
        spdlog::info("AgentCore: cancellation requested");
    }
}

// -- tool_decision ------------------------------------------------------------

void AgentCore::tool_decision(const std::string& call_id,
                              ToolDecision decision,
                              const nlohmann::json& modified_args)
{
    dispatcher_->submit_decision(call_id, decision, modified_args);
    spdlog::trace("AgentCore: tool_decision for {} = {}",
                  call_id,
                  decision == ToolDecision::approve ? "approve" :
                  decision == ToolDecision::reject  ? "reject"  : "modify");
}

// -- compact_context ----------------------------------------------------------

void AgentCore::compact_context(CompactionStrategy strategy, int n)
{
    pending_compact_strategy_.store(strategy, std::memory_order_relaxed);
    pending_compact_n_.store(n, std::memory_order_relaxed);
    pending_compact_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(queue_mutex_);
    }
    queue_cv_.notify_one();
    spdlog::info("AgentCore: compaction queued (strategy={}, n={})",
                 strategy == CompactionStrategy::drop_tool_results
                     ? "drop_tool_results" : "drop_oldest", n);
    activity_->emit(ActivityKind::compaction,
                    std::string("Compaction queued (") +
                        (strategy == CompactionStrategy::drop_tool_results
                            ? "drop_tool_results" : "drop_oldest") + ")",
                    "Will apply between LLM rounds or at end of turn.");
}

void AgentCore::run_compaction(const CompactionLayerSelection& selection,
                                int target_tokens,
                                const std::string& custom_instructions_override)
{
    {
        std::lock_guard lock(queue_mutex_);
        pending_pipeline_           = true;
        pending_pipeline_selection_ = selection;
        pending_pipeline_target_    = target_tokens;
        pending_pipeline_overrides_ = custom_instructions_override;
    }
    queue_cv_.notify_one();
    spdlog::info("AgentCore: pipeline compaction queued (target={}, override='{}')",
                 target_tokens, custom_instructions_override);
    activity_->emit(ActivityKind::compaction,
                    "Pipeline compaction queued",
                    "Will apply between LLM rounds or at end of turn.");
}

void AgentCore::observe_plan_tool_result(const ToolCall& call,
                                          const std::string& result_content,
                                          bool /*result_success*/)
{
    if (call.tool_name != "propose_plan" && call.tool_name != "mark_step_done")
        return;

    nlohmann::json j;
    try { j = nlohmann::json::parse(result_content); }
    catch (const nlohmann::json::exception& e) {
        spdlog::warn("AgentCore: plan tool '{}' returned non-JSON: {}",
                     call.tool_name, e.what());
        return;
    }

    bool ok = j.value("ok", false);
    if (!ok) {
        spdlog::info("AgentCore: plan tool '{}' reported failure: {}",
                     call.tool_name,
                     j.value("error", std::string{"(no message)"}));
        return;
    }

    if (call.tool_name == "propose_plan") {
        Plan p;
        {
            std::lock_guard lock(plan_mutex_);
            p.id = "plan-" + std::to_string(++plan_seq_);
        }
        p.title   = j.value("title", "");
        p.summary = j.value("summary", "");
        if (j.contains("steps") && j["steps"].is_array()) {
            for (auto& s : j["steps"]) {
                PlanStep step;
                step.description = s.value("description", "");
                if (s.contains("tools_needed") && s["tools_needed"].is_array()) {
                    for (auto& t : s["tools_needed"]) {
                        if (t.is_string())
                            step.tools_needed.push_back(t.get<std::string>());
                    }
                }
                step.status = PlanStep::Status::pending;
                p.steps.push_back(std::move(step));
            }
        }

        if (p.steps.empty()) {
            spdlog::warn("AgentCore: propose_plan accepted but plan has 0 steps");
            return;
        }

        Plan snapshot;
        {
            std::lock_guard lock(plan_mutex_);
            current_plan_ = p;
            plan_awaiting_decision_ = true;
            snapshot = *current_plan_;
        }
        spdlog::info("AgentCore: plan '{}' proposed ({} steps)",
                     snapshot.id, snapshot.steps.size());
        activity_->emit(ActivityKind::index_event,
                        "Plan proposed: " + snapshot.title,
                        std::to_string(snapshot.steps.size()) + " steps");
        frontends_.broadcast([&](IFrontend& fe) { fe.on_plan_proposed(snapshot); });
        return;
    }

    // mark_step_done
    int step_idx_1based = j.value("step", 0);
    std::string status_str = j.value("status", "done");
    std::string notes      = j.value("notes", "");

    PlanStep::Status new_status = PlanStep::Status::done;
    if (status_str == "failed") new_status = PlanStep::Status::failed;

    std::string plan_id;
    int idx = step_idx_1based - 1;
    bool advanced = false;
    bool completed = false;
    bool overall_success = true;
    {
        std::lock_guard lock(plan_mutex_);
        if (!current_plan_.has_value()) {
            spdlog::warn("AgentCore: mark_step_done with no active plan");
            return;
        }
        plan_id = current_plan_->id;
        if (idx < 0 || idx >= static_cast<int>(current_plan_->steps.size())) {
            spdlog::warn("AgentCore: mark_step_done index {} out of range",
                         step_idx_1based);
            return;
        }
        current_plan_->steps[idx].status = new_status;
        current_plan_->steps[idx].notes  = notes;
        advanced = true;
        completed = current_plan_->all_done();
        if (completed) {
            for (auto& s : current_plan_->steps) {
                if (s.status == PlanStep::Status::failed) {
                    overall_success = false;
                    break;
                }
            }
        }
    }

    if (!advanced) return;

    spdlog::info("AgentCore: plan '{}' step {} -> {}",
                 plan_id, step_idx_1based, to_string(new_status));
    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_plan_step_advanced(plan_id, idx, new_status, notes);
    });

    if (completed) {
        spdlog::info("AgentCore: plan '{}' completed (success={})",
                     plan_id, overall_success);
        activity_->emit(ActivityKind::index_event,
                        "Plan completed",
                        plan_id +
                          (overall_success ? " (all steps done)"
                                           : " (with failures)"));
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_plan_completed(plan_id, overall_success);
        });
    }
}

void AgentCore::apply_pending_compaction()
{
    // Drain new-pipeline request first; it supersedes legacy strategies.
    bool                     have_pipeline = false;
    CompactionLayerSelection pipeline_sel;
    int                      pipeline_target = 0;
    std::string              pipeline_overrides;
    {
        std::lock_guard lock(queue_mutex_);
        if (pending_pipeline_) {
            have_pipeline       = true;
            pipeline_sel        = std::move(pending_pipeline_selection_);
            pipeline_target     = pending_pipeline_target_;
            pipeline_overrides  = std::move(pending_pipeline_overrides_);
            pending_pipeline_   = false;
        }
    }

    if (have_pipeline) {
        if (!pipeline_overrides.empty())
            pipeline_sel.custom_summary_instructions = pipeline_overrides;

        // Default target = under warn_threshold of effective_limit.
        if (pipeline_target <= 0) {
            int eff = ctx_->effective_limit();
            double thr = 0.70;
            if (auto* ws = services_.workspace())
                thr = ws->config().compaction.warn_threshold;
            pipeline_target = static_cast<int>(eff * thr);
            if (pipeline_target <= 0) pipeline_target = eff > 0 ? eff / 2 : 1;
        }

        // Archive before mutating (chained backups).
        int archive_keep = 5;
        if (auto* ws = services_.workspace())
            archive_keep = ws->config().compaction.archive_keep_count;
        std::filesystem::path archive_path;
        if (history_archive_) {
            archive_path = history_archive_->snapshot(
                ctx_->session_id(), ctx_->history());
            history_archive_->gc(ctx_->session_id(), archive_keep);
        }
        int counter = 0;
        if (!archive_path.empty()) {
            counter = HistoryArchive::counter_from_filename(
                archive_path.filename().string());
        }

        int before_tokens = ctx_->history().estimate_tokens();
        auto result = CompactionPipeline::run(
            ctx_->history(), pipeline_sel, pipeline_target,
            &llm_, ctx_->llm_config());

        // Invalidate the budget's cached server total so the meter recomputes
        // against the new heuristic.
        ctx_->budget().set_server_total(0);
        ctx_->budget().set_server_split(0, 0);

        // Insert the footnote at index 1 (right after the system message) so
        // the volatile-tail rule from S4.F isn't broken. Only when an archive
        // was actually written.
        if (counter > 0) {
            std::string note = "[Earlier context compacted; full pre-compaction "
                               "history archived at " +
                               HistoryArchive::footnote_relative_path(
                                   ctx_->session_id(), counter) + "]";
            ChatMessage fn;
            fn.role    = MessageRole::user;
            fn.content = note;
            fn.token_estimate = TokenCounter::estimate_message(fn);

            // Splice in at index 1 (after the system prompt). We rebuild the
            // history vector to keep ConversationHistory's owner-thread fence
            // happy.
            std::vector<ChatMessage> rebuilt;
            const auto& msgs = ctx_->history().messages();
            rebuilt.reserve(msgs.size() + 1);
            for (std::size_t i = 0; i < msgs.size(); ++i) {
                rebuilt.push_back(msgs[i]);
                if (i == 0) rebuilt.push_back(fn);
            }
            ctx_->history().clear();
            for (auto& m : rebuilt) ctx_->history().add(std::move(m));
        }

        int after_tokens = ctx_->history().estimate_tokens();
        spdlog::info("AgentCore: pipeline compaction {} -> {} (target {}, reached={})",
                     before_tokens, after_tokens, pipeline_target,
                     result.reached_target ? "yes" : "no");

        std::ostringstream summary;
        summary << "Compaction applied: freed ~"
                << (before_tokens - after_tokens) << " tokens ("
                << before_tokens << " -> " << after_tokens << ")";
        if (!archive_path.empty())
            summary << "  archive=" << archive_path.filename().string();

        std::ostringstream detail;
        detail << "Pipeline result:\n";
        detail << "  before:  " << result.before_tokens << "\n";
        detail << "  after:   " << result.after_tokens << "\n";
        detail << "  target:  " << result.target_tokens
               << "  (" << (result.reached_target ? "reached" : "not reached") << ")\n";
        for (const auto& lr : result.layers) {
            detail << "  - " << lr.name
                   << ": " << lr.tokens_freed << " tokens freed, "
                   << lr.messages_touched << " message(s) touched";
            if (!lr.detail.empty()) detail << "  (" << lr.detail << ")";
            detail << "\n";
        }
        if (!archive_path.empty())
            detail << "Archive: " << archive_path.string() << "\n";

        activity_->emit(ActivityKind::compaction,
                        summary.str(), detail.str());
        std::string user_msg = summary.str() +
            (archive_path.empty() ? "" : "\nArchive: " + archive_path.string());
        frontends_.broadcast([&](IFrontend& fe) { fe.on_token(user_msg + "\n"); });

        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_context_meter(ctx_->current_tokens(), ctx_->llm_config().context_limit,
                                ctx_->budget().last_prompt_tokens(),
                                ctx_->budget().last_completion_tokens(),
                                ctx_->budget().reserve());
        });
        return;
    }

    if (!pending_compact_.exchange(false, std::memory_order_acq_rel))
        return;

    auto strategy = pending_compact_strategy_.load(std::memory_order_relaxed);
    int  n        = pending_compact_n_.load(std::memory_order_relaxed);

    int before = ctx_->history().estimate_tokens();

    switch (strategy) {
    case CompactionStrategy::drop_tool_results:
        ctx_->compact_drop_tool_results();
        break;
    case CompactionStrategy::drop_oldest:
        ctx_->compact_drop_oldest_turns(n > 0 ? n : 3);
        break;
    }

    int after = ctx_->history().estimate_tokens();
    spdlog::info("AgentCore: compaction freed ~{} tokens ({} -> {})",
                 before - after, before, after);

    activity_->emit(ActivityKind::compaction,
                    fmt::format("Compaction applied: freed ~{} tokens ({} -> {})",
                                before - after, before, after),
                    fmt::format(
                        "Strategy: {}\nN: {}\nBefore (history estimate): {} tokens\nAfter:  {} tokens",
                        strategy == CompactionStrategy::drop_tool_results
                            ? "drop_tool_results" : "drop_oldest",
                        n, before, after));

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(ctx_->current_tokens(), ctx_->llm_config().context_limit,
                            ctx_->budget().last_prompt_tokens(),
                            ctx_->budget().last_completion_tokens(),
                            ctx_->budget().reserve());
    });
}

// -- reset_conversation -------------------------------------------------------

void AgentCore::reset_conversation()
{
    // LLMContext handles: clear history + re-seed system prompt + reset
    // budget + drop checkpoint session + roll a fresh session id +
    // re-snapshot the file-change tracker.
    ctx_->reset_for_new_conversation();

    metrics_->reset();

    // S4.D -- reset mode + plan.
    AgentMode prev_mode = mode_.exchange(AgentMode::chat,
                                          std::memory_order_acq_rel);
    pending_mode_change_.store(false, std::memory_order_relaxed);
    pending_plan_approve_.store(false, std::memory_order_relaxed);
    pending_plan_reject_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(plan_mutex_);
        current_plan_.reset();
        plan_awaiting_decision_ = false;
        plan_seq_ = 0;
    }
    if (prev_mode != AgentMode::chat) {
        frontends_.broadcast([](IFrontend& fe) {
            fe.on_mode_changed(AgentMode::chat);
        });
    }

    spdlog::info("AgentCore: conversation reset");

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(ctx_->current_tokens(), ctx_->llm_config().context_limit,
                            ctx_->budget().last_prompt_tokens(),
                            ctx_->budget().last_completion_tokens(),
                            ctx_->budget().reserve());
    });
    frontends_.broadcast([](IFrontend& fe) { fe.on_session_reset(); });
}

// -- undo_turn / list_checkpoints (S4.B) -------------------------------------

std::string AgentCore::undo_turn(int turn_id)
{
    auto* cps = ctx_->checkpoints();
    if (!cps)
        return "Undo unavailable: checkpoint store not configured for this workspace.";

    int target = turn_id;
    if (target <= 0) {
        auto turns = cps->list_turns(ctx_->session_id());
        if (turns.empty())
            return "Nothing to undo: no checkpointed turns in this session.";
        target = turns.back().turn_id;
    }

    auto info = cps->read_turn(ctx_->session_id(), target);
    if (!info)
        return "Nothing to undo: turn " + std::to_string(target) +
               " has no checkpoint.";

    auto result = cps->restore_turn(ctx_->session_id(), target,
                                    services_.root());
    cps->drop_turn(ctx_->session_id(), target);

    std::ostringstream summary;
    summary << "Undo turn " << target << ": "
            << result.restored << " restored, "
            << result.deleted << " deleted, "
            << result.skipped << " skipped";
    if (!result.errors.empty()) {
        summary << "\nErrors:";
        for (const auto& e : result.errors) summary << "\n  - " << e;
    }

    activity_->emit(ActivityKind::index_event,
                    "Undid turn " + std::to_string(target),
                    summary.str());
    spdlog::info("AgentCore: {}", summary.str());
    return summary.str();
}

std::vector<TurnInfo> AgentCore::list_checkpoints() const
{
    auto* cps = ctx_->checkpoints();
    if (!cps) return {};
    return cps->list_turns(ctx_->session_id());
}

std::optional<std::string> AgentCore::read_current_pre_mutation(
    const std::string& rel_path) const
{
    return ctx_->read_current_pre_mutation(rel_path);
}

// -- Metrics export (S4.S) ---------------------------------------------------

std::string AgentCore::export_metrics(const std::string& format) const
{
    std::string fmt = format;
    std::transform(fmt.begin(), fmt.end(), fmt.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (fmt != "json" && fmt != "csv")
        return "Error: format must be 'json' or 'csv'";

    auto dir = services_.root() / ".locus" / "metrics";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return "Error: cannot create metrics directory: " + ec.message();

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream stamp;
    stamp << std::put_time(&tm, "%Y-%m-%d_%H%M%S");

    auto path = dir / ("metrics_" + stamp.str() + "." + fmt);
    std::ofstream f(path);
    if (!f.is_open())
        return "Error: cannot write " + path.string();

    if (fmt == "json")
        f << metrics_->to_json().dump(2) << '\n';
    else
        f << metrics_->to_csv();

    spdlog::info("AgentCore: metrics exported to {}", path.string());
    return path.string();
}

// -- save_session / load_session ---------------------------------------------

std::string AgentCore::save_session()
{
    nlohmann::json extras;
    extras["metrics"] = metrics_->to_json();
    return ctx_->save_session(extras);
}

void AgentCore::load_session(const std::string& session_id)
{
    ctx_->load_session(session_id);
    metrics_->reset();

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(ctx_->current_tokens(), ctx_->llm_config().context_limit,
                            ctx_->budget().last_prompt_tokens(),
                            ctx_->budget().last_completion_tokens(),
                            ctx_->budget().reserve());
    });
    frontends_.broadcast([](IFrontend& fe) { fe.on_session_reset(); });
}

// -- Slash commands (direct tool invocation) ---------------------------------

AgentCore::SlashOutcome AgentCore::try_slash_command(const std::string& content)
{
    auto handled = [] {
        SlashOutcome o; o.kind = SlashKind::handled; return o;
    };
    auto not_slash = [] {
        SlashOutcome o; o.kind = SlashKind::not_a_slash; return o;
    };

    auto trim_left = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        return std::string(s);
    };
    std::string s = trim_left(content);
    if (!s.empty() && s.front() == '/') {
        size_t end = s.find_first_of(" \t", 1);
        std::string cmd = s.substr(1, end == std::string::npos ? std::string::npos : end - 1);
        std::string rest = end == std::string::npos ? std::string{}
                                                    : trim_left(s.substr(end));
        if (cmd == "undo") {
            int target = 0;
            if (!rest.empty()) {
                try { target = std::stoi(rest); }
                catch (...) {
                    frontends_.broadcast([&](IFrontend& fe) {
                        fe.on_error("Usage: /undo [turn_id]");
                    });
                    return handled();
                }
            }
            std::string msg = undo_turn(target);
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(msg); });
            return handled();
        }
        if (cmd == "metrics") {
            auto agg = metrics_->aggregates();
            std::ostringstream o;
            o << "Metrics - " << agg.turn_count << " turn"
              << (agg.turn_count == 1 ? "" : "s") << "\n";
            o << "  tokens: in="   << agg.tokens_in_total
              << "  out="          << agg.tokens_out_total
              << "  reasoning="    << agg.reasoning_total << "\n";
            o << "  throughput: "  << std::fixed << std::setprecision(1)
              << agg.tokens_per_second << " tok/s ("
              << agg.stream_ms_total / 1000.0 << "s of stream)\n";
            o << "  turn time: avg=" << agg.avg_turn_ms << "ms"
              << " p95=" << agg.p95_turn_ms << "ms"
              << " max=" << agg.max_turn_ms << "ms\n";
            o << "  kv-cache: cached="    << agg.cached_tokens_total
              << "  ratio="    << std::fixed << std::setprecision(1)
              << (agg.cached_token_ratio * 100.0) << "%"
              << "  prefix_stable=" << (agg.prompt_prefix_stable ? "yes" : "no")
              << "\n";
            if (agg.last_ttft_ms > 0)
                o << "  prefill: ttft=" << agg.last_ttft_ms << "ms"
                  << "  ms/prompt_tok=" << std::fixed << std::setprecision(2)
                  << agg.last_prefill_ms_per_token << "\n";
            if (agg.retrieval_queries > 0)
                o << "  retrieval: " << agg.retrieval_queries
                  << " queries, hit_rate="
                  << std::fixed << std::setprecision(2)
                  << (agg.retrieval_hit_rate * 100.0) << "%\n";
            if (!agg.tool_calls_by_name.empty()) {
                o << "  tools:\n";
                for (auto& [n, c] : agg.tool_calls_by_name)
                    o << "    " << n << ": " << c << "\n";
            }
            std::string msg = o.str();
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(msg); });
            return handled();
        }
        if (cmd == "export_metrics") {
            std::string fmt = rest.empty() ? "json" : rest;
            std::string msg = export_metrics(fmt);
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(msg); });
            return handled();
        }
        if (cmd == "compact") {
            CompactionLayerSelection sel;
            if (auto* ws = services_.workspace()) {
                sel = selection_from_config(ws->config().compaction);
            }
            sel.drop_redundant_tool_results = true;
            sel.strip_large_tool_bodies     = true;
            sel.drop_old_reasoning          = true;
            sel.drop_oldest_turns           = false;
            sel.llm_summary                 = true;

            std::string override = rest;
            while (!override.empty()
                   && (override.front() == ' ' || override.front() == '\t'))
                override.erase(override.begin());
            while (!override.empty()
                   && (override.back() == ' ' || override.back() == '\t'))
                override.pop_back();

            run_compaction(sel, /*target=*/0, override);
            std::string msg = "/compact: cascade queued"
                              + std::string(override.empty() ? "" :
                                            " (custom instructions: " + override + ")")
                              + "\n";
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(msg); });
            return handled();
        }
        if (cmd == "reload") {
            if (!prompt_templates_) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error("Prompt templates are not configured for this workspace.");
                });
                return handled();
            }
            int n = 0;
            try { n = prompt_templates_->reload(); }
            catch (const std::exception& ex) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error(std::string("/reload failed: ") + ex.what());
                });
                return handled();
            }
            std::ostringstream m;
            m << "Reloaded prompt templates: " << n << " available.\n";
            frontends_.broadcast(
                [&](IFrontend& fe) { fe.on_token(m.str()); });
            return handled();
        }
        if (cmd == "forget") {
            auto* mem = services_.memory();
            if (!mem) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error("Memory bank is disabled for this workspace.");
                });
                return handled();
            }
            bool hard = false;
            std::string target_id = rest;
            auto strip_flag = [&](const std::string& flag) {
                auto p = target_id.find(flag);
                if (p == std::string::npos) return;
                size_t end = p + flag.size();
                target_id.erase(p, flag.size());
                while (p < target_id.size()
                       && (target_id[p] == ' ' || target_id[p] == '\t')) {
                    target_id.erase(p, 1);
                }
                while (p > 0 && (target_id[p - 1] == ' '
                                 || target_id[p - 1] == '\t')) {
                    target_id.erase(p - 1, 1);
                    --p;
                }
                (void)end;
            };
            if (target_id.find("--hard") != std::string::npos) {
                hard = true;
                strip_flag("--hard");
            }
            while (!target_id.empty()
                   && (target_id.front() == ' ' || target_id.front() == '\t'))
                target_id.erase(target_id.begin());
            while (!target_id.empty()
                   && (target_id.back() == ' ' || target_id.back() == '\t'))
                target_id.pop_back();

            if (target_id.empty()) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error("Usage: /forget <id> [--hard]");
                });
                return handled();
            }

            bool ok = hard ? mem->hard_delete(target_id)
                           : mem->soft_delete(target_id);
            if (!ok) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error("/forget: no memory entry with id '"
                                + target_id + "'");
                });
                return handled();
            }
            activity_->emit(ActivityKind::memory_deleted,
                            std::string("memory:") + target_id
                                + (hard ? " (hard)" : ""),
                            hard ? "Permanently removed."
                                 : "Moved to .deleted/ (30-day retention).");
            std::ostringstream msg;
            msg << "Forgot memory:" << target_id
                << (hard ? " (permanently)\n"
                         : " (moved to .deleted/; restore with the Settings panel)\n");
            frontends_.broadcast(
                [&](IFrontend& fe) { fe.on_token(msg.str()); });
            return handled();
        }
        if (cmd == "memorize") {
            auto* mem = services_.memory();
            if (!mem) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error("Memory bank is disabled for this workspace.");
                });
                return handled();
            }

            std::vector<std::string> tags;
            bool pinned = false;
            std::string content_body;

            size_t i = 0;
            while (i < rest.size()) {
                while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) ++i;
                if (i >= rest.size()) break;
                if (rest[i] == '+') {
                    size_t end_tok = rest.find_first_of(" \t", i);
                    if (end_tok == std::string::npos) end_tok = rest.size();
                    std::string tag = rest.substr(i + 1, end_tok - i - 1);
                    if (!tag.empty()) tags.push_back(std::move(tag));
                    i = end_tok;
                    continue;
                }
                if (rest.compare(i, 5, "--pin") == 0
                    && (i + 5 == rest.size() || rest[i + 5] == ' '
                                              || rest[i + 5] == '\t')) {
                    pinned = true;
                    i += 5;
                    continue;
                }
                content_body = rest.substr(i);
                break;
            }
            if (content_body.empty()) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error("Usage: /memorize [+tag1 +tag2] [--pin] <content>");
                });
                return handled();
            }

            try {
                std::string id = mem->add(content_body, tags, pinned, "user");
                std::ostringstream tag_str;
                for (size_t t = 0; t < tags.size(); ++t) {
                    if (t) tag_str << ",";
                    tag_str << tags[t];
                }
                std::ostringstream summary;
                summary << "memory:" << id;
                if (pinned) summary << " (pinned)";
                if (!tags.empty()) summary << " [" << tag_str.str() << "]";
                std::ostringstream detail;
                detail << "id: " << id << "\npinned: " << (pinned ? "true" : "false")
                       << "\ntags: [" << tag_str.str() << "]\n\n" << content_body;
                activity_->emit(ActivityKind::memory_added,
                                summary.str(), detail.str());

                std::ostringstream msg;
                msg << "Saved memory:" << id;
                if (pinned) msg << " (pinned)";
                if (!tags.empty()) msg << "  tags=" << tag_str.str();
                msg << "\n";
                frontends_.broadcast(
                    [&](IFrontend& fe) { fe.on_token(msg.str()); });
            } catch (const std::exception& ex) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error(std::string("/memorize failed: ") + ex.what());
                });
            }
            return handled();
        }

        // S4.X -- prompt-template lookup BEFORE handing off to the dispatcher.
        if (prompt_templates_ && !tools_.find(cmd)
            && prompt_templates_->has(cmd)) {
            std::vector<std::string>                     positional;
            std::unordered_map<std::string, std::string> kwargs;
            try {
                auto parsed = SlashCommandParser::parse(content, tools_);
                if (parsed) {
                    positional = std::move(parsed->positional);
                    kwargs     = std::move(parsed->kwargs);
                }
            } catch (const SlashParseError& ex) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error(std::string("Slash command error: ") + ex.what());
                });
                return handled();
            }

            std::string expanded;
            try {
                expanded = prompt_templates_->expand(cmd, positional, kwargs);
            } catch (const std::exception& ex) {
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error(std::string("Template expansion failed: ") + ex.what());
                });
                return handled();
            }

            if (ctx_->llm_config().context_limit > 0) {
                int est = TokenCounter::estimate(expanded);
                int half = ctx_->llm_config().context_limit / 2;
                if (est > half) {
                    std::ostringstream w;
                    w << "warning: prompt template '/" << cmd << "' expanded to ~"
                      << est << " tokens (>" << half
                      << ", half of context " << ctx_->llm_config().context_limit << ").";
                    std::string ws = w.str();
                    frontends_.broadcast(
                        [&](IFrontend& fe) { fe.on_error(ws); });
                }
            }

            activity_->emit(ActivityKind::user_message,
                            "Prompt template /" + cmd + " expanded ("
                                + std::to_string(expanded.size()) + " chars)",
                            expanded);

            SlashOutcome o;
            o.kind = SlashKind::expanded;
            o.expanded_text = std::move(expanded);
            return o;
        }
    }

    bool dispatched = slash_->try_dispatch(
        content,
        [this](std::string s) {
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(s); });
        },
        [this](std::string s) {
            frontends_.broadcast([&](IFrontend& fe) { fe.on_error(s); });
        });
    return dispatched ? handled() : not_slash();
}

} // namespace locus
