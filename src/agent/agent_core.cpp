#include "agent_core.h"

#include "index_query.h"
#include "tool_registry.h"
#include "workspace.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace locus {

AgentCore::AgentCore(ILLMClient& llm,
                     IToolRegistry& tools,
                     IWorkspaceServices& services,
                     const std::string& locus_md,
                     const WorkspaceMetadata& ws_meta,
                     const LLMConfig& llm_config,
                     const std::filesystem::path& sessions_dir)
    : llm_(llm)
    , tools_(tools)
    , services_(services)
    , llm_config_(llm_config)
    , sessions_(sessions_dir)
{
    base_system_prompt_ = SystemPromptBuilder::build(locus_md, ws_meta, tools);
    system_prompt_ = base_system_prompt_;

    activity_   = std::make_unique<ActivityLog>(frontends_);
    budget_     = std::make_unique<ContextBudget>(llm_config_.context_limit, frontends_);
    loop_       = std::make_unique<AgentLoop>(llm_, tools_, services_,
                                               *activity_, *budget_, frontends_);
    dispatcher_ = std::make_unique<ToolDispatcher>(tools_, services_, *activity_,
                                                   frontends_, cancel_requested_);
    slash_      = std::make_unique<SlashCommandDispatcher>(tools_, services_);

    int sys_tokens = ILLMClient::estimate_tokens(system_prompt_);
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

    history_.add({MessageRole::user, content});

    activity_->emit(ActivityKind::user_message,
                    "User message (" + std::to_string(content.size()) + " chars)",
                    content);

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(current_token_count(), llm_config_.context_limit);
    });

    int round = 0;
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

        if (step.had_error)
            break;

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
        spdlog::warn("AgentCore: hit max round limit ({})", k_max_rounds);
        frontends_.broadcast([](IFrontend& fe) {
            fe.on_error("Agent reached the maximum number of tool call rounds.");
        });
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
    spdlog::info("AgentCore: conversation reset");

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(current_token_count(), llm_config_.context_limit);
    });
    frontends_.broadcast([](IFrontend& fe) { fe.on_session_reset(); });
}

// -- save_session / load_session ---------------------------------------------

std::string AgentCore::save_session()
{
    auto id = sessions_.save(history_);
    spdlog::info("AgentCore: session saved as '{}'", id);
    return id;
}

void AgentCore::load_session(const std::string& session_id)
{
    history_ = sessions_.load(session_id);
    budget_->reset();
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
