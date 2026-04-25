#include "agent_core.h"

#include "index/index_query.h"
#include "llm/token_counter.h"
#include "tool_registry.h"
#include "workspace.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace locus {

// Generate a timestamp-based session id like SessionManager — keeps the on-
// disk layout under .locus/checkpoints/<session_id>/ readable and sortable.
static std::string make_session_id()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d_%H%M%S");
    return os.str();
}

AgentCore::AgentCore(ILLMClient& llm,
                     IToolRegistry& tools,
                     IWorkspaceServices& services,
                     const std::string& locus_md,
                     const WorkspaceMetadata& ws_meta,
                     const LLMConfig& llm_config,
                     const std::filesystem::path& sessions_dir,
                     const std::filesystem::path& checkpoints_dir)
    : llm_(llm)
    , tools_(tools)
    , services_(services)
    , llm_config_(llm_config)
    , sessions_(sessions_dir)
{
    base_system_prompt_ = SystemPromptBuilder::build(
        locus_md, ws_meta, tools, llm_config_.tool_format);
    system_prompt_ = base_system_prompt_;

    activity_   = std::make_unique<ActivityLog>(frontends_);
    budget_     = std::make_unique<ContextBudget>(llm_config_.context_limit, frontends_);
    metrics_    = std::make_unique<MetricsAggregator>();
    loop_       = std::make_unique<AgentLoop>(llm_, tools_, services_,
                                               *activity_, *budget_, frontends_,
                                               metrics_.get());
    dispatcher_ = std::make_unique<ToolDispatcher>(tools_, services_, *activity_,
                                                   frontends_, cancel_requested_,
                                                   metrics_.get());
    slash_      = std::make_unique<SlashCommandDispatcher>(tools_, services_);

    // S4.T -- file-change tracker. Always allocated; the per-turn injection is
    // gated by WorkspaceConfig::notify_external_changes. Take an initial
    // snapshot so the first user turn diffs against "the world as it was when
    // we started", not against an empty set (which would flood the prompt).
    change_tracker_ = std::make_unique<FileChangeTracker>();
    if (auto* idx = services_.index()) {
        try { change_tracker_->snapshot(*idx); }
        catch (const std::exception& e) {
            spdlog::warn("FileChangeTracker: initial snapshot failed: {}", e.what());
        }
    }
    dispatcher_->set_change_tracker(change_tracker_.get());

    session_id_ = make_session_id();
    turn_id_    = 0;
    if (!checkpoints_dir.empty()) {
        checkpoints_ = std::make_unique<CheckpointStore>(checkpoints_dir);
        spdlog::info("AgentCore: checkpoints at {} (session '{}')",
                     checkpoints_dir.string(), session_id_);
    }

    int sys_tokens = TokenCounter::estimate(system_prompt_);
    spdlog::info("AgentCore: system prompt ~{} tokens, context limit {}",
                 sys_tokens, llm_config_.context_limit);

    history_.add({MessageRole::system, system_prompt_});

    activity_->emit(ActivityKind::system_prompt,
                    "System prompt assembled (~" + std::to_string(sys_tokens) + " tokens)",
                    system_prompt_,
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

// -- Attached file context (S2.4) --------------------------------------------

std::string AgentCore::compose_system_prompt() const
{
    std::optional<AttachedContext> ctx;
    {
        std::lock_guard lock(attached_mutex_);
        ctx = attached_context_;
    }
    if (!ctx) return base_system_prompt_;

    std::ostringstream ss;
    ss << base_system_prompt_;
    ss << "\n## Attached File\n";
    ss << "[Attached: " << ctx->file_path << "]\n";

    if (auto* idx = services_.index()) {
        try {
            auto entries = idx->get_file_outline(ctx->file_path);
            if (!entries.empty()) {
                ss << "\nOutline:\n";
                for (const auto& e : entries) {
                    ss << "- L" << e.line << " ";
                    if (e.type == OutlineEntry::Heading) {
                        ss << std::string(static_cast<size_t>(std::max(1, e.level)), '#')
                           << " " << e.text;
                    } else {
                        ss << "[" << (e.kind.empty() ? "symbol" : e.kind) << "] "
                           << e.text;
                        if (!e.signature.empty())
                            ss << " " << e.signature;
                    }
                    ss << "\n";
                }
            }
        } catch (const std::exception& ex) {
            spdlog::warn("Attached-context outline lookup failed for '{}': {}",
                         ctx->file_path, ex.what());
        }
    }
    if (!ctx->preview.empty())
        ss << "\nPreview: " << ctx->preview << "\n";
    return ss.str();
}

void AgentCore::refresh_system_prompt()
{
    system_prompt_ = compose_system_prompt();
    if (!history_.empty() &&
        history_.messages().front().role == MessageRole::system) {
        history_.replace_system_prompt(system_prompt_);
    }
}

void AgentCore::set_attached_context(AttachedContext ctx)
{
    {
        std::lock_guard lock(attached_mutex_);
        attached_context_ = ctx;
    }
    refresh_system_prompt();
    spdlog::info("AgentCore: attached context set to '{}'", ctx.file_path);
    activity_->emit(ActivityKind::index_event,
                    "Attached file: " + ctx.file_path,
                    "Pinned to system prompt; outline injected on next turn.");

    auto snapshot = std::optional<AttachedContext>{ctx};
    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_attached_context_changed(snapshot);
    });
}

void AgentCore::clear_attached_context()
{
    bool had_value = false;
    {
        std::lock_guard lock(attached_mutex_);
        had_value = attached_context_.has_value();
        attached_context_.reset();
    }
    if (!had_value) return;

    refresh_system_prompt();
    spdlog::info("AgentCore: attached context cleared");
    activity_->emit(ActivityKind::index_event, "Detached file from context", "");

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_attached_context_changed(std::nullopt);
    });
}

std::optional<AttachedContext> AgentCore::attached_context() const
{
    std::lock_guard lock(attached_mutex_);
    return attached_context_;
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

        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [&] {
                return !message_queue_.empty() || !running_.load();
            });

            if (!running_.load())
                break;

            message = std::move(message_queue_.front());
            message_queue_.pop();
        }

        // Gate the history to this thread for the duration of the turn. S3.I:
        // inter-turn mutations (e.g. CLI REPL /reset) run with owner cleared.
        {
            ConversationOwnerScope owner_scope(history_);
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
}

void AgentCore::unregister_frontend(IFrontend* fe)
{
    frontends_.unregister_frontend(fe);
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

    if (try_slash_command(content)) {
        frontends_.broadcast([](IFrontend& fe) { fe.on_turn_complete(); });
        busy_.store(false);
        return;
    }

    // Bump the turn counter and arm the dispatcher to snapshot any mutating
    // tool call. Slash commands above don't bump — they bypass the dispatcher.
    ++turn_id_;
    if (checkpoints_) {
        dispatcher_->set_turn_context(checkpoints_.get(), session_id_, turn_id_);
        checkpoints_->gc_session(session_id_);
    }
    metrics_->begin_turn(turn_id_);

    // S4.T -- compute "files changed since last assistant turn" and prepend a
    // one-liner to the user message before it lands in history. The tracker's
    // suppression set carries paths the agent itself touched last turn; reset
    // it now so this turn's edits get recorded fresh.
    std::string effective_content = content;
    bool notify = true;
    if (auto* ws = services_.workspace())
        notify = ws->config().notify_external_changes;
    if (notify && change_tracker_) {
        if (auto* idx = services_.index()) {
            try {
                auto diff = change_tracker_->diff_since_snapshot(*idx);
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
        change_tracker_->clear_agent_touched();
    }

    history_.add({MessageRole::user, effective_content});

    activity_->emit(ActivityKind::user_message,
                    "User message (" + std::to_string(content.size()) + " chars)",
                    content);

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(current_token_count(), llm_config_.context_limit);
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

        if (budget_->check_overflow(current_token_count()))
            break;

        auto step = loop_->run_step(history_);

        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_context_meter(current_token_count(), llm_config_.context_limit);
        });

        if (step.had_error) {
            turn_had_error = true;
            break;
        }

        history_.add(std::move(step.assistant_msg));

        if (step.tool_calls.empty())
            break;

        for (auto& call : step.tool_calls) {
            if (cancel_requested_.load()) {
                spdlog::info("AgentCore: skipping remaining tool calls (cancelled)");
                break;
            }
            dispatcher_->dispatch(call, [this](ChatMessage m) {
                history_.add(std::move(m));
            });
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
    // accidentally tag a snapshot to the just-finished turn. (Inter-turn
    // mutations from outside the agent thread are rare, but we keep the
    // window tight.) If the turn touched no files, drop the (still-empty)
    // turn directory so /undo doesn't see phantom no-op turns.
    if (checkpoints_) {
        if (checkpoints_->entry_count(session_id_, turn_id_) == 0)
            checkpoints_->drop_turn(session_id_, turn_id_);
        dispatcher_->set_turn_context(nullptr, {}, 0);
    }

    // S4.T -- refresh the mtime baseline for next turn's diff. Done after the
    // assistant has finished so any writes the agent just made are part of
    // the new baseline (the suppression set is cleared next turn anyway).
    if (change_tracker_) {
        if (auto* idx = services_.index()) {
            try { change_tracker_->snapshot(*idx); }
            catch (const std::exception& e) {
                spdlog::warn("FileChangeTracker: end-of-turn snapshot failed: {}",
                             e.what());
            }
        }
    }

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
    dispatcher_->submit_decision(decision, modified_args);
    spdlog::trace("AgentCore: tool_decision for {} = {}",
                  call_id,
                  decision == ToolDecision::approve ? "approve" :
                  decision == ToolDecision::reject  ? "reject"  : "modify");
}

// -- compact_context ----------------------------------------------------------

void AgentCore::compact_context(CompactionStrategy strategy, int n)
{
    int before = history_.estimate_tokens();

    switch (strategy) {
    case CompactionStrategy::drop_tool_results:
        history_.drop_tool_results();
        break;
    case CompactionStrategy::drop_oldest:
        history_.drop_oldest_turns(n > 0 ? n : 3);
        break;
    }

    int after = history_.estimate_tokens();
    spdlog::info("AgentCore: compaction freed ~{} tokens ({} -> {})",
                 before - after, before, after);

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(current_token_count(), llm_config_.context_limit);
    });
}

// -- reset_conversation -------------------------------------------------------

void AgentCore::reset_conversation()
{
    history_.clear();
    system_prompt_ = compose_system_prompt();
    history_.add({MessageRole::system, system_prompt_});
    budget_->reset();
    metrics_->reset();

    // Drop checkpoint history with the conversation; start a fresh session
    // so the on-disk layout doesn't keep accumulating across resets.
    if (checkpoints_) {
        checkpoints_->drop_session(session_id_);
        session_id_ = make_session_id();
        turn_id_ = 0;
    }

    // S4.T -- baseline a fresh snapshot so we don't replay diffs that
    // happened across the now-cleared session. clear_agent_touched is implied
    // by the snapshot taking but be explicit.
    if (change_tracker_) {
        change_tracker_->clear_agent_touched();
        if (auto* idx = services_.index()) {
            try { change_tracker_->snapshot(*idx); }
            catch (const std::exception& e) {
                spdlog::warn("FileChangeTracker: reset snapshot failed: {}",
                             e.what());
            }
        }
    }

    spdlog::info("AgentCore: conversation reset");

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(current_token_count(), llm_config_.context_limit);
    });
    frontends_.broadcast([](IFrontend& fe) { fe.on_session_reset(); });
}

// -- undo_turn / list_checkpoints (S4.B) -------------------------------------

std::string AgentCore::undo_turn(int turn_id)
{
    if (!checkpoints_)
        return "Undo unavailable: checkpoint store not configured for this workspace.";

    // turn_id == 0 → resolve to the highest turn that actually has a manifest.
    int target = turn_id;
    if (target <= 0) {
        auto turns = checkpoints_->list_turns(session_id_);
        if (turns.empty())
            return "Nothing to undo: no checkpointed turns in this session.";
        target = turns.back().turn_id;
    }

    auto info = checkpoints_->read_turn(session_id_, target);
    if (!info)
        return "Nothing to undo: turn " + std::to_string(target) +
               " has no checkpoint.";

    auto result = checkpoints_->restore_turn(session_id_, target,
                                             services_.root());
    // Drop the manifest so the same turn can't be undone twice and
    // list_checkpoints() reflects the current state of the world.
    checkpoints_->drop_turn(session_id_, target);

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
    if (!checkpoints_) return {};
    return checkpoints_->list_turns(session_id_);
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
    auto id = sessions_.save(history_, extras);
    spdlog::info("AgentCore: session saved as '{}'", id);
    return id;
}

void AgentCore::load_session(const std::string& session_id)
{
    history_ = sessions_.load(session_id);
    budget_->reset();
    metrics_->reset();
    spdlog::info("AgentCore: loaded session '{}' ({} messages)",
                 session_id, history_.size());

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(current_token_count(), llm_config_.context_limit);
    });
    frontends_.broadcast([](IFrontend& fe) { fe.on_session_reset(); });
}

// -- Token accounting --------------------------------------------------------

int AgentCore::current_token_count() const
{
    return budget_->current(history_.estimate_tokens());
}

// -- Slash commands (direct tool invocation) ---------------------------------
// Parsing + execution live in SlashCommandDispatcher (S3.J). AgentCore only
// routes the output to frontends.

bool AgentCore::try_slash_command(const std::string& content)
{
    // Built-in non-tool slash commands handled directly by AgentCore.
    // /undo  → revert the last checkpointed turn (or a specific id).
    auto trim_left = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        return std::string(s);
    };
    std::string s = trim_left(content);
    if (!s.empty() && s.front() == '/') {
        // Split "/cmd rest"
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
                    return true;
                }
            }
            std::string msg = undo_turn(target);
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(msg); });
            return true;
        }
        if (cmd == "metrics") {
            auto agg = metrics_->aggregates();
            std::ostringstream o;
            o << "Metrics — " << agg.turn_count << " turn"
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
            return true;
        }
        if (cmd == "export_metrics") {
            std::string fmt = rest.empty() ? "json" : rest;
            std::string msg = export_metrics(fmt);
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(msg); });
            return true;
        }
    }

    return slash_->try_dispatch(
        content,
        [this](std::string s) {
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(s); });
        },
        [this](std::string s) {
            frontends_.broadcast([&](IFrontend& fe) { fe.on_error(s); });
        });
}

} // namespace locus
