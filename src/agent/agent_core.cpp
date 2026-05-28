#include "agent_core.h"

#include "agent_turn.h"
#include "auto_commit.h"
#include "compaction_pipeline.h"
#include "history_archive.h"
#include "quality_monitor.h"
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
    // S6.11 -- pass the workspace's lazy_tool_manifest flag so the assembly
    // renders summaries (not per-param lines) when on, and so describe_tool's
    // available(ws) filter affects the prompt as well as the API tools array.
    // S6.12 -- pass the system_prompt_profile so the prose body (Rules /
    // Editing / Shell / MSVC pitfalls) renders at the chosen verbosity.
    bool lazy_manifest = false;
    SystemPromptProfile profile = SystemPromptProfile::Full;
    if (auto* w = services_.workspace()) {
        lazy_manifest = w->config().agent.lazy_tool_manifest;
        profile       = profile_from_string(w->config().agent.system_prompt_profile);
    }
    auto assembly = SystemPromptAssembly::build(
        locus_md, ws_meta, tools, llm_config.tool_format, memory_section,
        lazy_manifest, &services_, profile);

    int sys_tokens = assembly.total_tokens();
    spdlog::info("AgentCore: system prompt ~{} tokens (profile={}), context limit {}",
                 sys_tokens, to_string(profile), llm_config.context_limit);

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
        // Push current context-meter state so the chip shows the baseline cost
        // (system prompt + tool manifest) immediately rather than 0/0 until
        // the first user turn fires on_context_meter.
        fe->on_context_meter(ctx_->current_tokens(),
                             ctx_->llm_config().context_limit,
                             ctx_->budget().last_prompt_tokens(),
                             ctx_->budget().last_completion_tokens(),
                             ctx_->budget().reserve());
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
    // S6.13 -- reset per-turn watchdog state. nudge_requested_ can survive
    // across turns only if a frontend bug fires it while not busy; defensive.
    nudge_requested_.store(false);
    nudges_this_turn_ = 0;
    // S6.17 Task B.1 -- reset per-turn compaction escalation streak.
    compaction_no_op_streak_ = 0;

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
        notify = ws->config().agent.notify_external_changes;
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

    // Per-workspace cap (Agentic Tetris findings #5). 0 = unbounded.
    int max_rounds = 0;
    if (auto* ws = services_.workspace())
        max_rounds = ws->config().agent.max_rounds_per_message;

    bool turn_had_error = AgentTurnRunner(*this, turn_id, max_rounds).run().turn_had_error;

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
            if (cfg.git.auto_commit) {
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
                    cfg.git.commit_prefix,
                    cfg.git.commit_branch,
                    assistant_text);

                if (r.success && !r.skipped) {
                    activity_->emit(ActivityKind::index_event,
                                    "Auto-commit " + r.short_sha
                                        + " on " + r.branch,
                                    cfg.git.commit_prefix
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

void AgentCore::request_commit_now()
{
    if (!busy_.load()) return;
    // Set BOTH flags. cancel_requested_ breaks the LLM stream cleanly via
    // the existing path in AgentLoop. nudge_requested_ is the differentiator
    // that tells the round loop "this was a commit-now, not a full Stop --
    // inject the steering message and keep going". Order matters: set the
    // nudge flag first so the cancel-handling branch in the round loop sees
    // it as a consistent pair.
    nudge_requested_.store(true);
    cancel_requested_.store(true);
    dispatcher_->wake();
    spdlog::info("AgentCore: commit-now requested (manual)");
}

// -- inject_nudge (S6.10 Task B) ----------------------------------------------

bool AgentCore::inject_nudge(std::string body, const std::string& reason_tag)
{
    // Shares the 2-cap with the reasoning watchdog -- a turn that fires
    // both detectors gets at most two corrections in total, never two of
    // each. Once the third would-fire arrives, callers should treat the
    // false return as a signal to abort the turn.
    if (++nudges_this_turn_ > 2) {
        spdlog::warn("AgentCore: nudge cap hit (reason={}); refusing further "
                     "corrections this turn", reason_tag);
        return false;
    }
    int tok = 0;
    ctx_->add_message({MessageRole::user, body});
    tok = ctx_->history().messages().back().token_estimate;
    activity_->emit(
        ActivityKind::quality_correction,
        "Quality nudge (#" + std::to_string(nudges_this_turn_)
            + ", reason=" + reason_tag + ")",
        body,
        /*tokens_in=*/tok);
    spdlog::info("AgentCore: quality-monitor nudge #{} injected (reason={})",
                 nudges_this_turn_, reason_tag);
    return true;
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

        // S5.Z task 6 -- notify frontends so the chat-footer chip can read
        // "compacted: N". Fires only when an archive was actually written
        // (counter > 0); GC trims older archives but the latest snapshot's
        // counter is monotonically increasing, so it equals the on-disk
        // count *as observed by the chip*, even after GC drops earlier ones.
        if (counter > 0) {
            int no_op_total = compaction_no_op_total_;
            frontends_.broadcast([&, no_op_total](IFrontend& fe) {
                fe.on_compaction_archived(counter, no_op_total);
            });
        }

        // S6.17 Task B.1 -- escalation. After N consecutive reached=no
        // compactions within this turn, force-enable the next layer in the
        // fixed order, overriding the snapshot selection. Independent of the
        // user's chosen aggressiveness preset -- saturation is a runtime
        // signal that the configured layers aren't matching the conversation
        // shape (e.g. many small tool results with no large bodies to strip).
        const int escalation_steps = compaction_no_op_streak_;
        if (escalation_steps >= 1) {
            pipeline_sel.drop_redundant_tool_results = true;
            pipeline_sel.strip_large_tool_bodies     = true;
        }
        if (escalation_steps >= 2) {
            pipeline_sel.drop_old_reasoning = true;
            pipeline_sel.llm_summary        = true;
        }
        if (escalation_steps >= 3) {
            pipeline_sel.drop_oldest_turns = true;
        }
        if (escalation_steps > 0) {
            spdlog::warn("CompactionPipeline: escalating to step {} "
                         "after {} consecutive no-op compaction(s) this turn",
                         escalation_steps, compaction_no_op_streak_);
        }

        int before_tokens = ctx_->history().estimate_tokens();
        auto result = CompactionPipeline::run(
            ctx_->history(), pipeline_sel, pipeline_target,
            &llm_, ctx_->llm_config());

        // S6.18 Task B.2 -- when the cascade ran with drop_oldest_turns
        // enabled and still couldn't reach target, fall back to hard-trim:
        // delete the oldest turn group (between system[0] + first-user and
        // the most recent kept block) one turn at a time until target is
        // hit. The cascade's drop_candidates honours the preservation
        // heuristic; the hard-trim path doesn't. This is the floor that
        // makes "every turn is essential" workspaces (zero-stripping shape)
        // forward-progressable instead of stuck forever.
        if (!result.reached_target && pipeline_sel.drop_oldest_turns) {
            std::vector<ChatMessage> msgs = ctx_->history().messages();
            int now = TokenCounter::estimate(msgs);
            int hard_trimmed = 0;
            while (now > pipeline_target) {
                // Collect user-message indices; need at least 3 user
                // messages so we can drop the 2nd's group while keeping
                // system[0], the 1st user (session seed), and a recent
                // user message. Two users -> nothing to hard-trim;
                // user-less history -> can't establish boundaries.
                std::vector<std::size_t> users;
                for (std::size_t i = 0; i < msgs.size(); ++i) {
                    if (msgs[i].role == MessageRole::user) users.push_back(i);
                }
                if (users.size() < 3) break;
                std::size_t drop_begin = users[1];
                std::size_t drop_end   = users[2];
                msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(drop_begin),
                           msgs.begin() + static_cast<ptrdiff_t>(drop_end));
                ++hard_trimmed;
                now = TokenCounter::estimate(msgs);
            }
            if (hard_trimmed > 0) {
                spdlog::warn("CompactionPipeline: saturated, hard-trimmed {} turn(s) "
                             "(target {}, before-hardtrim {}, after-hardtrim {})",
                             hard_trimmed, pipeline_target,
                             result.after_tokens, now);
                ctx_->history().clear();
                for (auto& m : msgs) ctx_->history().add(std::move(m));
                result.after_tokens   = now;
                result.reached_target = (now <= pipeline_target);
                activity_->emit(ActivityKind::compaction,
                                "Compaction hard-trim (saturated)",
                                "trimmed_turns=" + std::to_string(hard_trimmed) +
                                " before_hardtrim=" + std::to_string(before_tokens) +
                                " after_hardtrim=" + std::to_string(now) +
                                " target=" + std::to_string(pipeline_target));
            }
        }

        // Invalidate the budget's cached server total so the meter recomputes
        // against the new heuristic.
        ctx_->budget().set_server_total(0);
        ctx_->budget().set_server_split(0, 0);

        // Insert the footnote at index 1 (right after the system message) so
        // the volatile-tail rule from S4.F isn't broken. Only when an archive
        // was actually written.
        if (counter > 0) {
            // S6.18 Task B.1 -- progress-preserving sentinel. When Layer 6
            // produced a summary, prepend it as "Progress so far: <summary>"
            // so the post-compaction model reads the breadcrumb instead of
            // a context-free "history was compacted" line.
            std::string note;
            const std::string& progress = result.llm_summary_text;
            const std::string archive_rel =
                HistoryArchive::footnote_relative_path(ctx_->session_id(), counter);
            if (!progress.empty()) {
                note  = "[Earlier context compacted. Progress so far: ";
                note += progress;
                if (!note.empty() && note.back() != '.' && note.back() != '\n')
                    note += ".";
                note += " Full pre-compaction history archived at " + archive_rel + "]";
            } else {
                note = "[Earlier context compacted; full pre-compaction "
                       "history archived at " + archive_rel + "]";
            }
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
        // S6.17 Task B.4 -- promote reached=no to warn the first time, error
        // on a second consecutive no-op so the log is the canonical signal
        // for "compaction pipeline isn't keeping up".
        if (!result.reached_target) {
            ++compaction_no_op_streak_;
            ++compaction_no_op_total_;
            if (compaction_no_op_streak_ >= 2) {
                spdlog::error("AgentCore: pipeline compaction {} -> {} "
                              "(target {}, reached=no) -- {}th consecutive no-op this turn",
                              before_tokens, after_tokens, pipeline_target,
                              compaction_no_op_streak_);
            } else {
                spdlog::warn("AgentCore: pipeline compaction {} -> {} (target {}, reached=no)",
                             before_tokens, after_tokens, pipeline_target);
            }
            activity_->emit(ActivityKind::compaction,
                            "Compaction no-op (target not reached)",
                            "before=" + std::to_string(before_tokens) +
                            " after=" + std::to_string(after_tokens) +
                            " target=" + std::to_string(pipeline_target) +
                            " streak=" + std::to_string(compaction_no_op_streak_));
        } else {
            compaction_no_op_streak_ = 0;
            spdlog::info("AgentCore: pipeline compaction {} -> {} (target {}, reached=yes)",
                         before_tokens, after_tokens, pipeline_target);
        }

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

int AgentCore::compacted_archive_count(const std::string& session_id) const
{
    if (!history_archive_ || session_id.empty()) return 0;
    return history_archive_->highest_counter(session_id);
}

// -- save_session / load_session ---------------------------------------------

std::string AgentCore::save_session()
{
    nlohmann::json extras;
    extras["metrics"] = metrics_->to_json();

    // Persist the surrounding agent state so re-opening a saved session
    // restores not just the chat transcript but the mode + plan the user
    // had in flight. `agent_mode` always written; `plan` only when a plan
    // is currently live (plan-mode preview / execute-mode progress).
    extras["agent_mode"] = to_string(mode_.load());
    std::optional<Plan> plan_snapshot;
    bool awaiting = false;
    {
        std::lock_guard<std::mutex> g(plan_mutex_);
        plan_snapshot = current_plan_;
        awaiting      = plan_awaiting_decision_;
    }
    if (plan_snapshot.has_value()) {
        extras["plan"]                    = plan_to_json(*plan_snapshot);
        extras["plan_awaiting_decision"]  = awaiting;
    }

    std::string id = ctx_->save_session(extras);

    // Sidecar JSONL activity log -- opt-in via WorkspaceConfig::Sessions::
    // persist_activity. Set the persist path on the live ActivityLog so
    // every subsequent emit() flows to disk. Re-saving an existing session
    // points at the same sidecar path (append-only), so future activity
    // continues the existing log file.
    if (auto* ws = services_.workspace()) {
        if (ws->config().sessions.persist_activity && !id.empty()) {
            auto path = sessions_.sessions_dir() / (id + ".activity.jsonl");
            activity_->set_persist_path(path);
        }
    }

    return id;
}

void AgentCore::load_session(const std::string& session_id)
{
    // Read the full session file before applying so we can pick up the
    // top-level `agent_mode` / `plan` block alongside the messages. The
    // history-only load() path stays the cheap default for callers that
    // don't need mode/plan restore.
    nlohmann::json full;
    try {
        full = sessions_.load_full(session_id);
    } catch (const std::exception& e) {
        // Fall back to history-only load so the legacy path still works
        // even if the meta read fails.
        spdlog::warn("AgentCore::load_session: full-load failed ({}), "
                     "falling back to history-only", e.what());
    }

    ctx_->load_session(session_id);
    metrics_->reset();

    // Activity sidecar replay (opt-in). Done AFTER on_session_reset would
    // have been broadcast otherwise -- but we wait to broadcast it below
    // because the activity panel needs to clear its existing list before
    // we replay events into it. Clear the in-memory ring + reset the
    // persist path first so a previously-open session doesn't bleed events
    // into the freshly-loaded one.
    activity_->clear();
    activity_->set_persist_path({});
    if (auto* ws = services_.workspace()) {
        if (ws->config().sessions.persist_activity) {
            auto path = sessions_.sessions_dir()
                        / (session_id + ".activity.jsonl");
            activity_->replay_from(path);
            activity_->set_persist_path(path);
        }
    }

    // Re-apply persisted mode + plan. Done after history load so the
    // chat-panel render path (which fires from on_session_reset below)
    // sees the right mode banner / plan bubble. We use the public setters
    // so the existing pending-thread-hop machinery + frontend broadcasts
    // keep working: set_mode queues onto the agent thread; AgentLoop's
    // apply_pending_mode_change picks it up on the next idle tick.
    AgentMode restored_mode = AgentMode::chat;
    std::optional<Plan> restored_plan;
    bool restored_awaiting = false;
    if (full.is_object()) {
        if (full.contains("agent_mode") && full["agent_mode"].is_string()) {
            restored_mode = agent_mode_from_string(
                full["agent_mode"].get<std::string>());
        }
        if (full.contains("plan") && full["plan"].is_object()) {
            restored_plan = plan_from_json(full["plan"]);
            restored_awaiting = full.value("plan_awaiting_decision", false);
        }
    }

    {
        std::lock_guard<std::mutex> g(plan_mutex_);
        current_plan_           = restored_plan;
        plan_awaiting_decision_ = restored_awaiting;
    }
    // Use direct store-and-broadcast (rather than the queued set_mode path)
    // because the agent thread is idle here -- we want the new state visible
    // before on_session_reset fires so the chat panel paints the right
    // mode tab + plan bubble on first render.
    mode_.store(restored_mode);
    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_mode_changed(restored_mode);
    });
    if (restored_plan.has_value()) {
        Plan plan_copy = *restored_plan;
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_plan_proposed(plan_copy);
        });
    }

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
        if (cmd == "breakdown") {
            // Per-message token table. Tells the user where their context
            // budget is actually going -- the chat usually surfaces only the
            // "visible" assistant + user bubbles, but the system prompt and
            // tool-call argument payloads are often the actual whales. Uses
            // TokenCounter so the per-row numbers reconcile with the
            // chat-footer ctx counter (modulo the volatile attached-context
            // / file-change prepends, which aren't in `history()`).
            const auto& msgs = ctx_->history().messages();
            std::ostringstream o;
            o << "Context breakdown -- " << msgs.size() << " message"
              << (msgs.size() == 1 ? "" : "s")
              << ", ~" << ctx_->history().estimate_tokens() << " tokens\n";
            o << "(estimator: ~4 chars/tok + 4 framing/msg)\n\n";
            o << "```\n";
            o << "  #  role        toks   chars  preview\n";
            o << "  ---------------------------------------------------------------\n";
            int total = 0;
            // First pass -- compute and collect for the top-contributors view.
            std::vector<std::pair<int,int>> rows;  // (msg_idx, tokens)
            rows.reserve(msgs.size());
            for (std::size_t i = 0; i < msgs.size(); ++i) {
                const auto& m = msgs[i];
                int toks  = TokenCounter::estimate_message(m);
                int chars = static_cast<int>(m.content.size());
                for (auto& tc : m.tool_calls) {
                    chars += static_cast<int>(tc.name.size() + tc.arguments.size());
                }
                total += toks;
                rows.emplace_back(static_cast<int>(i), toks);

                // Role label
                const char* role_str = "?";
                switch (m.role) {
                    case MessageRole::system:    role_str = "system";    break;
                    case MessageRole::user:      role_str = "user";      break;
                    case MessageRole::assistant: role_str = "assistant"; break;
                    case MessageRole::tool:      role_str = "tool";      break;
                }

                // Preview: prefer content; on tool-call assistants with no
                // content, show "[tool_call: name1, name2]" so the user sees
                // which tools are responsible for the payload.
                std::string preview;
                if (m.content.empty() && !m.tool_calls.empty()) {
                    preview = "[tool_call: ";
                    for (std::size_t k = 0; k < m.tool_calls.size(); ++k) {
                        if (k) preview += ", ";
                        preview += m.tool_calls[k].name;
                    }
                    preview += "]";
                } else {
                    preview = m.content;
                }
                // Collapse newlines + cap at 60 chars so the table stays
                // single-line per row.
                for (auto& c : preview) if (c == '\n' || c == '\r') c = ' ';
                if (preview.size() > 60) preview = preview.substr(0, 57) + "...";

                o << "  " << std::setw(2) << i
                  << "  " << std::left << std::setw(10) << role_str << std::right
                  << "  " << std::setw(5) << toks
                  << "  " << std::setw(6) << chars
                  << "  " << preview << "\n";
            }
            o << "  ---------------------------------------------------------------\n";
            o << "  total: ~" << total << " tokens\n";
            o << "```\n";

            // Top contributors: any single message that ate > 5% of total.
            if (total > 0) {
                std::vector<std::pair<int,int>> top = rows;
                std::sort(top.begin(), top.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
                bool any = false;
                for (auto& [idx, toks] : top) {
                    double pct = (toks * 100.0) / total;
                    if (pct < 5.0) break;
                    if (!any) {
                        o << "\nTop contributors (>5%):\n";
                        any = true;
                    }
                    const auto& m = msgs[idx];
                    const char* role_str = "?";
                    switch (m.role) {
                        case MessageRole::system:    role_str = "system";    break;
                        case MessageRole::user:      role_str = "user";      break;
                        case MessageRole::assistant: role_str = "assistant"; break;
                        case MessageRole::tool:      role_str = "tool";      break;
                    }
                    o << "  #" << idx << " " << role_str
                      << "  " << toks << " tok  "
                      << std::fixed << std::setprecision(0) << pct << "%\n";
                }
            }

            // Volatile prepends: attached-context block + file-change notes
            // are spliced onto the next user message and never appear in
            // `history()`. Surface their cost separately so the user can
            // reconcile this breakdown with the chat-footer counter.
            std::string attached_block = ctx_->compose_attached_context_block();
            int attached_toks = attached_block.empty()
                ? 0
                : TokenCounter::estimate(attached_block);
            o << "\nVolatile prepends (not in history above, added next turn):\n";
            o << "  attached context: ~" << attached_toks << " tokens\n";

            std::string msg = o.str();
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
