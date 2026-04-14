#include "agent_core.h"
#include "tool_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace locus {

AgentCore::AgentCore(ILLMClient& llm,
                     IToolRegistry& tools,
                     const WorkspaceContext& ws_context,
                     const std::string& locus_md,
                     const WorkspaceMetadata& ws_meta,
                     const LLMConfig& llm_config)
    : llm_(llm)
    , tools_(tools)
    , ws_context_(ws_context)
    , llm_config_(llm_config)
{
    system_prompt_ = SystemPromptBuilder::build(locus_md, ws_meta, tools);

    int sys_tokens = ILLMClient::estimate_tokens(system_prompt_);
    spdlog::info("AgentCore: system prompt ~{} tokens, context limit {}",
                 sys_tokens, llm_config_.context_limit);

    // Seed conversation with the system message.
    history_.add({MessageRole::system, system_prompt_});
}

// -- Frontend registration ----------------------------------------------------

void AgentCore::register_frontend(IFrontend* fe)
{
    std::lock_guard lock(frontends_mutex_);
    frontends_.push_back(fe);
    spdlog::trace("AgentCore: frontend registered ({} total)", frontends_.size());
}

void AgentCore::unregister_frontend(IFrontend* fe)
{
    std::lock_guard lock(frontends_mutex_);
    frontends_.erase(std::remove(frontends_.begin(), frontends_.end(), fe),
                     frontends_.end());
    spdlog::trace("AgentCore: frontend unregistered ({} remain)", frontends_.size());
}

// -- send_message (main entry point) ------------------------------------------

void AgentCore::send_message(const std::string& content)
{
    spdlog::info("AgentCore: user message ({} chars)", content.size());

    history_.add({MessageRole::user, content});
    broadcast_context_meter();

    // Agent loop: call LLM, handle tool calls, repeat until text-only response.
    int round = 0;
    static constexpr int k_max_rounds = 20;

    while (round < k_max_rounds) {
        ++round;
        spdlog::trace("AgentCore: LLM round {}", round);

        if (check_context_overflow())
            break;

        bool has_tool_calls = run_llm_step();
        broadcast_context_meter();

        if (!has_tool_calls)
            break;
    }

    if (round >= k_max_rounds) {
        spdlog::warn("AgentCore: hit max round limit ({})", k_max_rounds);
        broadcast_error("Agent reached the maximum number of tool call rounds.");
    }

    broadcast_message_complete();
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
    broadcast_context_meter();
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
            broadcast_token(token);
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
            broadcast_error(err);
        }
    });

    if (had_error)
        return false;

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
        auto call = ToolRegistry::parse_tool_call(tc_req.id, tc_req.name, tc_req.arguments);
        auto* tool = tools_.find(call.tool_name);

        if (!tool) {
            spdlog::warn("AgentCore: unknown tool '{}'", call.tool_name);
            // Inject error result so LLM knows the call failed.
            ChatMessage err_result;
            err_result.role = MessageRole::tool;
            err_result.tool_call_id = call.id;
            err_result.content = "Error: unknown tool '" + call.tool_name + "'";
            history_.add(std::move(err_result));
            broadcast_error("Unknown tool: " + call.tool_name);
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

    ToolCall effective_call = call;

    if (tool->approval_policy() == "auto") {
        // Auto-approve: execute immediately.
        spdlog::trace("AgentCore: auto-approve '{}'", call.tool_name);
    } else {
        // Needs user approval.
        std::string preview = tool->preview(call);
        broadcast_tool_call_pending(call, preview);

        // Wait for decision.
        std::unique_lock lock(decision_mutex_);
        decision_cv_.wait(lock, [&] { return pending_decision_.has_value(); });

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
            broadcast_tool_result(call.id, "[rejected]");
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

    // Inject tool result into conversation.
    ChatMessage tool_msg;
    tool_msg.role = MessageRole::tool;
    tool_msg.tool_call_id = call.id;
    tool_msg.content = result.content;
    history_.add(std::move(tool_msg));

    broadcast_tool_result(call.id, result.display);
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

// -- Broadcast helpers --------------------------------------------------------

void AgentCore::broadcast_token(std::string_view token)
{
    std::lock_guard lock(frontends_mutex_);
    for (auto* fe : frontends_)
        fe->on_token(token);
}

void AgentCore::broadcast_tool_call_pending(const ToolCall& call,
                                            const std::string& preview)
{
    std::lock_guard lock(frontends_mutex_);
    for (auto* fe : frontends_)
        fe->on_tool_call_pending(call, preview);
}

void AgentCore::broadcast_tool_result(const std::string& call_id,
                                      const std::string& display)
{
    std::lock_guard lock(frontends_mutex_);
    for (auto* fe : frontends_)
        fe->on_tool_result(call_id, display);
}

void AgentCore::broadcast_message_complete()
{
    std::lock_guard lock(frontends_mutex_);
    for (auto* fe : frontends_)
        fe->on_message_complete();
}

void AgentCore::broadcast_context_meter()
{
    int used = history_.estimate_tokens();
    int limit = llm_config_.context_limit;
    std::lock_guard lock(frontends_mutex_);
    for (auto* fe : frontends_)
        fe->on_context_meter(used, limit);
}

void AgentCore::broadcast_compaction_needed()
{
    int used = history_.estimate_tokens();
    int limit = llm_config_.context_limit;
    std::lock_guard lock(frontends_mutex_);
    for (auto* fe : frontends_)
        fe->on_compaction_needed(used, limit);
}

void AgentCore::broadcast_error(const std::string& msg)
{
    std::lock_guard lock(frontends_mutex_);
    for (auto* fe : frontends_)
        fe->on_error(msg);
}

// -- Context overflow check ---------------------------------------------------

bool AgentCore::check_context_overflow()
{
    int used = history_.estimate_tokens();
    int limit = llm_config_.context_limit;
    double ratio = static_cast<double>(used) / limit;

    if (ratio >= 1.0) {
        spdlog::warn("AgentCore: context FULL ({}/{} tokens, {:.0f}%)",
                     used, limit, ratio * 100);
        broadcast_compaction_needed();
        return true;
    }

    if (ratio >= k_compaction_threshold) {
        spdlog::warn("AgentCore: context at {:.0f}% ({}/{} tokens)",
                     ratio * 100, used, limit);
        broadcast_compaction_needed();
        // Don't stop — let the LLM try, but warn.
    }

    return false;
}

} // namespace locus
