#include "agent_loop.h"

#include "activity_log.h"
#include "context_budget.h"
#include "frontend.h"
#include "tool_registry.h"

#include <spdlog/spdlog.h>

namespace locus {

AgentLoop::AgentLoop(ILLMClient& llm,
                     IToolRegistry& tools,
                     ActivityLog& activity,
                     ContextBudget& budget,
                     FrontendRegistry& frontends)
    : llm_(llm)
    , tools_(tools)
    , activity_(activity)
    , budget_(budget)
    , frontends_(frontends)
{}

std::vector<ToolSchema> AgentLoop::build_tool_schemas() const
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

AgentStepResult AgentLoop::run_step(const ConversationHistory& history)
{
    AgentStepResult out;
    auto tool_schemas = build_tool_schemas();

    std::string accumulated_text;
    std::string accumulated_reasoning;
    std::vector<ToolCallRequest> tool_call_requests;
    int reasoning_tokens_reported = 0;

    llm_.stream_completion(history.messages(), tool_schemas, {
        /* on_token */
        [&](const std::string& token) {
            accumulated_text += token;
            frontends_.broadcast([&](IFrontend& fe) { fe.on_token(token); });
        },
        /* on_reasoning_token */
        [&](const std::string& token) {
            accumulated_reasoning += token;
            frontends_.broadcast([&](IFrontend& fe) { fe.on_reasoning_token(token); });
        },
        /* on_tool_calls */
        [&](const std::vector<ToolCallRequest>& calls) {
            tool_call_requests = calls;
        },
        /* on_complete */
        [&]() {
            spdlog::trace("AgentLoop: LLM step complete, text={} chars, "
                          "reasoning={} chars, tool_calls={}",
                          accumulated_text.size(), accumulated_reasoning.size(),
                          tool_call_requests.size());
        },
        /* on_error */
        [&](const std::string& err) {
            out.had_error = true;
            frontends_.broadcast([&](IFrontend& fe) { fe.on_error(err); });
            activity_.emit(ActivityKind::error, "LLM error", err);
        },
        /* on_usage */
        [&](const CompletionUsage& u) {
            budget_.set_server_total(u.total_tokens);
            reasoning_tokens_reported = u.reasoning_tokens;
            spdlog::trace("AgentLoop: server reports {} total tokens "
                          "(prompt={}, completion={}, reasoning={})",
                          u.total_tokens, u.prompt_tokens, u.completion_tokens,
                          u.reasoning_tokens);
        }
    });

    if (out.had_error)
        return out;

    // Emit LLM-response activity with token accounting. Prefer server-reported
    // usage; if the backend omits it, fall back to the heuristic estimate of
    // the full conversation so the column is never empty.
    {
        int total = budget_.server_total();
        bool from_server = (total > 0);
        if (!from_server)
            total = history.estimate_tokens();

        int delta = total - budget_.prev_turn_total();

        std::string summary = "LLM response (total=" + std::to_string(total);
        if (delta != 0)
            summary += (delta > 0 ? ", +" : ", ") + std::to_string(delta);
        summary += from_server ? " tokens" : " tokens est.";
        if (reasoning_tokens_reported > 0)
            summary += ", reasoning=" + std::to_string(reasoning_tokens_reported);
        summary += ")";

        std::string detail;
        if (!accumulated_reasoning.empty()) {
            detail += "[thinking]\n" + accumulated_reasoning + "\n\n";
        }
        detail += accumulated_text;
        if (!tool_call_requests.empty()) {
            detail += "\n\n[tool_calls: ";
            for (size_t i = 0; i < tool_call_requests.size(); ++i) {
                if (i) detail += ", ";
                detail += tool_call_requests[i].name;
            }
            detail += "]";
        }
        activity_.emit(ActivityKind::llm_response,
                       std::move(summary),
                       std::move(detail),
                       /*tokens_in=*/total,
                       /*tokens_out=*/std::nullopt,
                       /*tokens_delta=*/delta);
        budget_.mark_turn_total(total);
    }

    out.assistant_msg.role = MessageRole::assistant;
    out.assistant_msg.content = accumulated_text;
    out.assistant_msg.tool_calls = tool_call_requests;

    out.tool_calls.reserve(tool_call_requests.size());
    for (auto& tc_req : tool_call_requests) {
        out.tool_calls.push_back(
            ToolRegistry::parse_tool_call(tc_req.id, tc_req.name, tc_req.arguments));
    }

    return out;
}

} // namespace locus
