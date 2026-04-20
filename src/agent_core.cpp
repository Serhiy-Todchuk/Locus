#include "agent_core.h"
#include "tool_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <sstream>

namespace locus {

AgentCore::AgentCore(ILLMClient& llm,
                     IToolRegistry& tools,
                     const WorkspaceContext& ws_context,
                     const std::string& locus_md,
                     const WorkspaceMetadata& ws_meta,
                     const LLMConfig& llm_config,
                     const std::filesystem::path& sessions_dir)
    : llm_(llm)
    , tools_(tools)
    , ws_context_(ws_context)
    , llm_config_(llm_config)
    , sessions_(sessions_dir)
{
    system_prompt_ = SystemPromptBuilder::build(locus_md, ws_meta, tools);

    int sys_tokens = ILLMClient::estimate_tokens(system_prompt_);
    spdlog::info("AgentCore: system prompt ~{} tokens, context limit {}",
                 sys_tokens, llm_config_.context_limit);

    // Seed conversation with the system message.
    history_.add({MessageRole::system, system_prompt_});

    // Record initial system prompt as the first activity event so users can
    // see what context was seeded and how many tokens it cost.
    emit_activity(ActivityKind::system_prompt,
                  "System prompt assembled (~" + std::to_string(sys_tokens) + " tokens)",
                  system_prompt_,
                  /*tokens_in=*/std::nullopt,
                  /*tokens_out=*/std::nullopt,
                  /*tokens_delta=*/sys_tokens);
}

// -- Activity emission -------------------------------------------------------

void AgentCore::emit_activity(ActivityKind kind,
                              std::string summary,
                              std::string detail,
                              std::optional<int> tokens_in,
                              std::optional<int> tokens_out,
                              std::optional<int> tokens_delta)
{
    ActivityEvent ev;
    {
        std::lock_guard lock(activity_mutex_);
        ev.id = next_activity_id_++;
        ev.timestamp = std::chrono::system_clock::now();
        ev.kind = kind;
        ev.summary = std::move(summary);
        ev.detail = std::move(detail);
        ev.tokens_in = tokens_in;
        ev.tokens_out = tokens_out;
        ev.tokens_delta = tokens_delta;

        activity_buffer_.push_back(ev);
        if (activity_buffer_.size() > k_activity_buffer_max) {
            activity_buffer_.erase(
                activity_buffer_.begin(),
                activity_buffer_.begin() +
                    (activity_buffer_.size() - k_activity_buffer_max));
        }
    }

    frontends_.broadcast([&](IFrontend& fe) { fe.on_activity(ev); });
}

std::vector<ActivityEvent> AgentCore::get_activity(uint64_t since_id) const
{
    std::lock_guard lock(activity_mutex_);
    std::vector<ActivityEvent> out;
    out.reserve(activity_buffer_.size());
    for (auto& ev : activity_buffer_)
        if (ev.id > since_id)
            out.push_back(ev);
    return out;
}

void AgentCore::emit_index_event(const std::string& summary, const std::string& detail)
{
    emit_activity(ActivityKind::index_event, summary, detail);
}

AgentCore::~AgentCore()
{
    stop();
}

// -- Start / Stop -------------------------------------------------------------

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

    // Wake the agent thread so it can exit.
    queue_cv_.notify_one();
    // Also wake decision wait in case it's blocked on tool approval.
    decision_cv_.notify_one();

    if (agent_thread_.joinable())
        agent_thread_.join();

    spdlog::info("AgentCore: agent thread stopped");
}

// -- Agent thread -------------------------------------------------------------

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

        process_message(message);

        // Signal sync waiters that the turn is done.
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

// -- send_message (non-blocking) ----------------------------------------------

void AgentCore::send_message(const std::string& content)
{
    spdlog::info("AgentCore: queuing user message ({} chars)", content.size());

    {
        std::lock_guard lock(queue_mutex_);
        message_queue_.push(content);
    }
    queue_cv_.notify_one();
}

// -- send_message_sync (blocking, for CLI) ------------------------------------

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

// -- process_message (runs on agent thread) -----------------------------------

void AgentCore::process_message(const std::string& content)
{
    spdlog::info("AgentCore: processing user message ({} chars)", content.size());

    busy_.store(true);
    cancel_requested_.store(false);

    frontends_.broadcast([](IFrontend& fe) { fe.on_turn_start(); });

    // Check for /slash commands (direct tool invocation).
    if (try_slash_command(content)) {
        frontends_.broadcast([](IFrontend& fe) { fe.on_turn_complete(); });
        busy_.store(false);
        return;
    }

    history_.add({MessageRole::user, content});

    {
        std::string sum = "User message (" + std::to_string(content.size()) + " chars)";
        emit_activity(ActivityKind::user_message, std::move(sum), content);
    }

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(current_token_count(), llm_config_.context_limit);
    });

    // Agent loop: call LLM, handle tool calls, repeat until text-only response.
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

        if (check_context_overflow())
            break;

        bool has_tool_calls = run_llm_step();

        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_context_meter(current_token_count(), llm_config_.context_limit);
        });

        if (!has_tool_calls)
            break;
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

// -- is_busy / cancel_turn ----------------------------------------------------

bool AgentCore::is_busy() const
{
    return busy_.load();
}

void AgentCore::cancel_turn()
{
    if (busy_.load()) {
        cancel_requested_.store(true);
        // Wake decision wait so we don't hang on tool approval.
        decision_cv_.notify_one();
        spdlog::info("AgentCore: cancellation requested");
    }
}

// -- tool_decision ------------------------------------------------------------

void AgentCore::tool_decision(const std::string& call_id,
                              ToolDecision decision,
                              const nlohmann::json& modified_args)
{
    std::lock_guard lock(decision_mutex_);
    pending_decision_ = decision;
    pending_modified_args_ = modified_args;
    decision_cv_.notify_one();
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
    history_.add({MessageRole::system, system_prompt_});
    last_server_total_tokens_ = 0;
    spdlog::info("AgentCore: conversation reset");

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(current_token_count(), llm_config_.context_limit);
    });
    frontends_.broadcast([](IFrontend& fe) { fe.on_session_reset(); });
}

// -- save_session / load_session -----------------------------------------------

std::string AgentCore::save_session()
{
    auto id = sessions_.save(history_);
    spdlog::info("AgentCore: session saved as '{}'", id);
    return id;
}

void AgentCore::load_session(const std::string& session_id)
{
    history_ = sessions_.load(session_id);
    last_server_total_tokens_ = 0;
    spdlog::info("AgentCore: loaded session '{}' ({} messages)",
                 session_id, history_.size());

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_context_meter(current_token_count(), llm_config_.context_limit);
    });
    frontends_.broadcast([](IFrontend& fe) { fe.on_session_reset(); });
}

// -- run_llm_step -------------------------------------------------------------

bool AgentCore::run_llm_step()
{
    auto tool_schemas = build_tool_schemas();

    std::string accumulated_text;
    std::vector<ToolCallRequest> tool_call_requests;
    bool has_tool_calls = false;
    bool had_error = false;

    llm_.stream_completion(history_.messages(), tool_schemas, {
        /* on_token */
        [&](const std::string& token) {
            accumulated_text += token;
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(token); });
        },
        /* on_tool_calls */
        [&](const std::vector<ToolCallRequest>& calls) {
            has_tool_calls = true;
            tool_call_requests = calls;
        },
        /* on_complete */
        [&]() {
            spdlog::trace("AgentCore: LLM step complete, text={} chars, tool_calls={}",
                          accumulated_text.size(), tool_call_requests.size());
        },
        /* on_error */
        [&](const std::string& err) {
            had_error = true;
            frontends_.broadcast([&](IFrontend& fe) { fe.on_error(err); });
            emit_activity(ActivityKind::error, "LLM error", err);
        },
        /* on_usage */
        [&](const CompletionUsage& u) {
            last_server_total_tokens_ = u.total_tokens;
            spdlog::trace("AgentCore: server reports {} total tokens (prompt={}, completion={})",
                          u.total_tokens, u.prompt_tokens, u.completion_tokens);
        }
    });

    if (had_error)
        return false;

    // Emit LLM-response activity with token accounting. Prefer server-reported
    // usage; if the backend (e.g. older LM Studio) omits it, fall back to the
    // heuristic estimate of the full conversation so the column is never empty.
    {
        int total = last_server_total_tokens_;
        bool from_server = (total > 0);
        if (!from_server)
            total = history_.estimate_tokens();

        int delta = total - prev_turn_total_tokens_;

        std::string summary = "LLM response (total=" + std::to_string(total);
        if (delta != 0)
            summary += (delta > 0 ? ", +" : ", ") + std::to_string(delta);
        summary += from_server ? " tokens)" : " tokens est.)";

        std::string detail = accumulated_text;
        if (!tool_call_requests.empty()) {
            detail += "\n\n[tool_calls: ";
            for (size_t i = 0; i < tool_call_requests.size(); ++i) {
                if (i) detail += ", ";
                detail += tool_call_requests[i].name;
            }
            detail += "]";
        }
        emit_activity(ActivityKind::llm_response, std::move(summary), std::move(detail),
                      /*tokens_in=*/total,
                      /*tokens_out=*/std::nullopt,
                      /*tokens_delta=*/delta);
        prev_turn_total_tokens_ = total;
    }

    // Add assistant message to history.
    ChatMessage assistant_msg;
    assistant_msg.role = MessageRole::assistant;
    assistant_msg.content = accumulated_text;
    assistant_msg.tool_calls = tool_call_requests;
    history_.add(std::move(assistant_msg));

    if (!has_tool_calls)
        return false;

    // Process each tool call.
    for (auto& tc_req : tool_call_requests) {
        if (cancel_requested_.load()) {
            spdlog::info("AgentCore: skipping remaining tool calls (cancelled)");
            break;
        }

        auto call = ToolRegistry::parse_tool_call(tc_req.id, tc_req.name, tc_req.arguments);
        auto* tool = tools_.find(call.tool_name);

        if (!tool) {
            spdlog::warn("AgentCore: unknown tool '{}'", call.tool_name);
            ChatMessage err_result;
            err_result.role = MessageRole::tool;
            err_result.tool_call_id = call.id;
            err_result.content = "Error: unknown tool '" + call.tool_name + "'";
            history_.add(std::move(err_result));
            frontends_.broadcast([&](IFrontend& fe) {
                fe.on_error("Unknown tool: " + call.tool_name);
            });
            emit_activity(ActivityKind::error,
                          "Unknown tool: " + call.tool_name,
                          "LLM requested tool '" + call.tool_name + "' which is not registered.");
            continue;
        }

        process_tool_call(call, tool);
    }

    return true;
}

// -- process_tool_call --------------------------------------------------------

void AgentCore::process_tool_call(const ToolCall& call, ITool* tool)
{
    spdlog::info("AgentCore: tool call '{}' id={}", call.tool_name, call.id);
    spdlog::trace("AgentCore: tool args: {}", call.args.dump());

    emit_activity(ActivityKind::tool_call,
                  "Tool call: " + call.tool_name,
                  "id: " + call.id + "\nargs: " + call.args.dump(2));

    ToolCall effective_call = call;

    if (tool->approval_policy() == "auto") {
        spdlog::trace("AgentCore: auto-approve '{}'", call.tool_name);
    } else {
        // Needs user approval.
        std::string preview = tool->preview(call);
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_tool_call_pending(call, preview);
        });

        // Wait for decision (or cancellation).
        std::unique_lock lock(decision_mutex_);
        decision_cv_.wait(lock, [&] {
            return pending_decision_.has_value() || cancel_requested_.load();
        });

        if (cancel_requested_.load() && !pending_decision_.has_value()) {
            // Cancelled while waiting for approval — treat as reject.
            lock.unlock();
            ChatMessage reject_result;
            reject_result.role = MessageRole::tool;
            reject_result.tool_call_id = call.id;
            reject_result.content = "Tool call cancelled.";
            history_.add(std::move(reject_result));
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
            spdlog::info("AgentCore: tool '{}' rejected by user", call.tool_name);
            ChatMessage reject_result;
            reject_result.role = MessageRole::tool;
            reject_result.tool_call_id = call.id;
            reject_result.content = "Tool call rejected by user.";
            history_.add(std::move(reject_result));
            frontends_.broadcast([&](IFrontend& fe) {
                fe.on_tool_result(call.id, "[rejected]");
            });
            return;
        }

        if (decision == ToolDecision::modify && !mod_args.empty()) {
            effective_call.args = mod_args;
            spdlog::info("AgentCore: tool '{}' args modified by user", call.tool_name);
        }
    }

    // Execute.
    auto result = tool->execute(effective_call, ws_context_);

    spdlog::info("AgentCore: tool '{}' result: success={}, content={} chars",
                 call.tool_name, result.success, result.content.size());
    spdlog::trace("AgentCore: tool result content: {}", result.content);

    {
        std::string sum = "Tool result: " + call.tool_name;
        sum += result.success ? " (ok)" : " (failed)";
        sum += " — " + std::to_string(result.content.size()) + " chars";
        emit_activity(result.success ? ActivityKind::tool_result : ActivityKind::error,
                      std::move(sum),
                      result.content);
    }

    // Inject tool result into conversation.
    ChatMessage tool_msg;
    tool_msg.role = MessageRole::tool;
    tool_msg.tool_call_id = call.id;
    tool_msg.content = result.content;
    history_.add(std::move(tool_msg));

    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_tool_result(call.id, result.display);
    });
}

// -- build_tool_schemas -------------------------------------------------------

std::vector<ToolSchema> AgentCore::build_tool_schemas() const
{
    std::vector<ToolSchema> schemas;
    auto schema_json = tools_.build_schema_json();

    for (auto& entry : schema_json) {
        if (!entry.contains("function")) continue;
        auto& fn = entry["function"];
        ToolSchema ts;
        ts.name        = fn.value("name", "");
        ts.description = fn.value("description", "");
        ts.parameters  = fn.value("parameters", nlohmann::json::object());
        schemas.push_back(std::move(ts));
    }

    return schemas;
}

// -- Token count --------------------------------------------------------------

int AgentCore::current_token_count() const
{
    // Prefer server-reported usage when available.
    if (last_server_total_tokens_ > 0)
        return last_server_total_tokens_;
    return history_.estimate_tokens();
}

// -- Context overflow check ---------------------------------------------------

bool AgentCore::check_context_overflow()
{
    int used = current_token_count();
    int limit = llm_config_.context_limit;
    double ratio = static_cast<double>(used) / limit;

    if (ratio >= 1.0) {
        spdlog::warn("AgentCore: context FULL ({}/{} tokens, {:.0f}%)",
                     used, limit, ratio * 100);
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_compaction_needed(used, limit);
        });
        return true;
    }

    if (ratio >= k_compaction_threshold) {
        spdlog::warn("AgentCore: context at {:.0f}% ({}/{} tokens)",
                     ratio * 100, used, limit);
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_compaction_needed(used, limit);
        });
    }

    return false;
}

// -- Slash commands (direct tool invocation) ----------------------------------

bool AgentCore::try_slash_command(const std::string& content)
{
    if (content.empty() || content[0] != '/')
        return false;

    // Parse: /tool_name [positional_arg] [key=value ...]
    // Special: /help lists all available tools.
    std::istringstream iss(content.substr(1));  // skip '/'
    std::string tool_name;
    iss >> tool_name;

    if (tool_name.empty())
        return false;

    // /help — list all available slash commands.
    if (tool_name == "help") {
        std::string help = "Available /commands (direct tool invocation):\n\n";
        for (auto* t : tools_.all()) {
            help += "  /" + t->name();
            for (auto& p : t->params()) {
                if (p.required)
                    help += " <" + p.name + ">";
                else
                    help += " [" + p.name + "=" + p.type + "]";
            }
            help += "\n    " + t->description() + "\n\n";
        }
        help += "  /help\n    Show this help\n";
        frontends_.broadcast([&](IFrontend& fe) { fe.on_token(help); });
        return true;
    }

    // Find the tool.
    auto* tool = tools_.find(tool_name);
    if (!tool) {
        std::string err = "Unknown command '/" + tool_name + "'. Type /help for available commands.";
        frontends_.broadcast([&](IFrontend& fe) { fe.on_error(err); });
        return true;
    }

    // Parse remaining args: first collect the rest of the line.
    std::string rest;
    if (iss.tellg() != -1)
        rest = content.substr(static_cast<size_t>(iss.tellg()) + 1);  // +1 for '/'

    // Trim leading whitespace.
    auto trim_start = rest.find_first_not_of(" \t");
    if (trim_start != std::string::npos)
        rest = rest.substr(trim_start);
    else
        rest.clear();

    // Build args JSON.
    // Tokenizer: respects "quoted strings" for multi-word values.
    // Syntax:  /tool positional_arg key=value key="multi word value"
    //          /tool "positional with spaces" key=value
    auto tool_params = tool->params();
    nlohmann::json args = nlohmann::json::object();

    // Tokenize rest into shell-like tokens (handles double quotes).
    std::vector<std::string> tokens;
    {
        size_t i = 0;
        while (i < rest.size()) {
            while (i < rest.size() && rest[i] == ' ') ++i;
            if (i >= rest.size()) break;

            std::string tok;
            if (rest[i] == '"') {
                // Quoted token: consume until closing quote.
                ++i;
                while (i < rest.size() && rest[i] != '"')
                    tok += rest[i++];
                if (i < rest.size()) ++i;  // skip closing quote
            } else {
                // Unquoted token: consume until space or key="...
                while (i < rest.size() && rest[i] != ' ') {
                    if (rest[i] == '"') {
                        // key="value with spaces"
                        ++i;
                        while (i < rest.size() && rest[i] != '"')
                            tok += rest[i++];
                        if (i < rest.size()) ++i;
                    } else {
                        tok += rest[i++];
                    }
                }
            }
            tokens.push_back(std::move(tok));
        }
    }

    // Separate into named (key=value) and positional args.
    std::vector<std::string> positional;
    for (auto& tok : tokens) {
        auto eq = tok.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = tok.substr(0, eq);
            std::string val = tok.substr(eq + 1);
            // Match param type for proper JSON conversion.
            bool found = false;
            for (auto& p : tool_params) {
                if (p.name == key) {
                    if (p.type == "integer")
                        args[key] = std::stoi(val);
                    else if (p.type == "boolean")
                        args[key] = (val == "true" || val == "1");
                    else
                        args[key] = val;
                    found = true;
                    break;
                }
            }
            if (!found)
                args[key] = val;
        } else {
            positional.push_back(tok);
        }
    }

    // Map positional args to params in declaration order.
    size_t pos_idx = 0;
    for (auto& p : tool_params) {
        if (args.contains(p.name)) continue;
        if (pos_idx < positional.size()) {
            if (p.type == "integer")
                args[p.name] = std::stoi(positional[pos_idx]);
            else if (p.type == "boolean")
                args[p.name] = (positional[pos_idx] == "true" || positional[pos_idx] == "1");
            else
                args[p.name] = positional[pos_idx];
            ++pos_idx;
        }
    }

    // Execute with timing.
    ToolCall call;
    call.id = "slash_" + tool_name;
    call.tool_name = tool_name;
    call.args = args;

    spdlog::info("Slash command: /{} args={}", tool_name, args.dump());

    auto t0 = std::chrono::steady_clock::now();
    auto result = tool->execute(call, ws_context_);
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // Format output.
    std::string output = "**/" + tool_name + "** ";
    if (result.success)
        output += "(OK, " + std::to_string(ms) + "ms)\n\n";
    else
        output += "(FAILED, " + std::to_string(ms) + "ms)\n\n";

    // Prefer display text, fall back to content.
    if (!result.display.empty())
        output += result.display;
    else
        output += result.content;

    frontends_.broadcast([&](IFrontend& fe) { fe.on_token(output); });
    return true;
}

} // namespace locus
