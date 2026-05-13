#include "agent_loop.h"

#include "activity_log.h"
#include "context_budget.h"
#include "../core/frontend.h"
#include "llm/token_counter.h"
#include "metrics.h"
#include "../tools/tool_registry.h"
#include "../core/workspace.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace locus {

AgentLoop::AgentLoop(ILLMClient& llm,
                     IToolRegistry& tools,
                     IWorkspaceServices& services,
                     ActivityLog& activity,
                     ContextBudget& budget,
                     FrontendRegistry& frontends,
                     std::atomic<bool>& cancel_flag,
                     MetricsAggregator* metrics)
    : llm_(llm)
    , tools_(tools)
    , services_(services)
    , activity_(activity)
    , budget_(budget)
    , frontends_(frontends)
    , cancel_flag_(cancel_flag)
    , metrics_(metrics)
{}

int AgentLoop::manifest_warn_tokens() const
{
    if (auto* ws = services_.workspace())
        return ws->config().tool_manifest_warn_tokens;
    return 4000;  // default if no Workspace (tests, embedded use)
}

std::vector<ToolSchema> AgentLoop::build_tool_schemas(ToolMode mode)
{
    // S3.L: filter the manifest to tools that are available in this workspace
    // and visible in the current mode. S4.D introduced plan/execute modes.
    auto schema_json = tools_.build_schema_json(services_, mode);

    std::vector<ToolSchema> schemas;
    schemas.reserve(schema_json.size());

    for (auto& entry : schema_json) {
        if (!entry.contains("function")) continue;
        auto& fn = entry["function"];
        ToolSchema ts;
        ts.name        = fn.value("name", "");
        ts.description = fn.value("description", "");
        ts.parameters  = fn.value("parameters", nlohmann::json::object());
        schemas.push_back(std::move(ts));
    }

    // Token-cost guardrail: log the manifest footprint every round, warn when
    // it exceeds the configured threshold. Serialised JSON length is a close
    // proxy for what the backend actually sends as the tools parameter.
    std::string schema_dump   = schema_json.dump();
    int         manifest_tokens = TokenCounter::estimate(schema_dump);
    int         threshold       = manifest_warn_tokens();
    bool        over_budget     = (manifest_tokens > threshold);
    if (over_budget) {
        spdlog::warn("Tool manifest: {} tools, ~{} tokens (threshold {}). "
                     "Consider trimming or splitting by mode.",
                     schemas.size(), manifest_tokens, threshold);
    } else {
        spdlog::info("Tool manifest: {} tools, ~{} tokens",
                     schemas.size(), manifest_tokens);
    }

    // M5 polish -- dedupe activity emits on a hash of the manifest content.
    // The manifest is sent to the LLM on every round (OpenAI API contract),
    // but a row in the Activity log for each one buried tool_call /
    // tool_result pairs between identical entries. Emit only when the
    // content changes (mode switch, MCP server crash, MCP restart, ...).
    // Over-budget warnings still fire every round so a runaway manifest
    // can't sneak past the noise filter.
    std::size_t manifest_hash = std::hash<std::string>{}(schema_dump);
    bool        first_emit    = (last_manifest_hash_ == 0);
    bool        changed       = (manifest_hash != last_manifest_hash_);
    if (over_budget || first_emit || changed) {
        std::string summary = "Tool manifest: " +
                              std::to_string(schemas.size()) + " tools, ~" +
                              std::to_string(manifest_tokens) + " tokens";
        if (changed && !first_emit) summary += " (changed)";
        std::string detail;
        for (std::size_t i = 0; i < schemas.size(); ++i) {
            if (i > 0) detail += '\n';
            detail += schemas[i].name;
        }
        ActivityKind kind = over_budget ? ActivityKind::warning
                                        : ActivityKind::tool_manifest;
        activity_.emit(kind, std::move(summary), std::move(detail),
                       /*tokens_in=*/manifest_tokens);
        last_manifest_hash_ = manifest_hash;
    }

    return schemas;
}

AgentStepResult AgentLoop::run_step(const ConversationHistory& history,
                                    ToolMode tool_mode)
{
    AgentStepResult out;
    auto tool_schemas = build_tool_schemas(tool_mode);

    // S4.F -- hash the outgoing system message before the POST. The system
    // message is the leading byte sequence the prefix cache locks onto;
    // logging the hash next to prompt_tokens / cached_tokens makes
    // cache-miss root causes obvious from .locus/locus.log alone.
    std::size_t sys_hash = 0;
    int         sys_chars = 0;
    if (!history.empty() &&
        history.messages().front().role == MessageRole::system) {
        const auto& sys_content = history.messages().front().content;
        sys_hash  = std::hash<std::string>{}(sys_content);
        sys_chars = static_cast<int>(sys_content.size());
        if (metrics_)
            metrics_->record_prompt_prefix_hash(sys_hash);
    }

    std::string accumulated_text;
    std::string accumulated_reasoning;
    std::vector<ToolCallRequest> tool_call_requests;
    int reasoning_tokens_reported = 0;
    CompletionUsage usage_reported{};
    int prev_total_for_metrics = budget_.prev_turn_total();
    auto stream_t0 = std::chrono::steady_clock::now();
    bool first_token_seen = false;
    long long ttft_ms = 0;

    // S4.F generation-progress throttle: 150 ms between callbacks. The
    // frontend receives accumulated chars + an estimate of tokens via the
    // shared ~4 chars/token heuristic. The exact completion_tokens still
    // arrive in the final on_usage and the frontend can reconcile.
    auto last_progress = std::chrono::steady_clock::time_point{};
    auto maybe_fire_progress = [&](bool force) {
        auto now = std::chrono::steady_clock::now();
        if (!force) {
            auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - last_progress).count();
            if (since < 150) return;
        }
        last_progress = now;
        int chars = static_cast<int>(accumulated_text.size()
                                       + accumulated_reasoning.size());
        // Same heuristic as TokenCounter::estimate but avoids a dependency
        // round-trip; ~4 chars per token.
        int est_tokens = (chars + 2) / 4;
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_generation_progress(chars, est_tokens);
        });
    };
    auto mark_first_token = [&]() {
        if (first_token_seen) return;
        first_token_seen = true;
        ttft_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - stream_t0).count();
        if (metrics_)
            metrics_->record_first_token(ttft_ms);
    };

    StreamCallbacks cbs;
    cbs.on_token = [&](const std::string& token) {
        mark_first_token();
        accumulated_text += token;
        // Don't render tokens after the user clicked Stop. The HTTP abort
        // happens at the next chunk boundary in the transport layer;
        // suppressing here avoids painting up to that boundary's worth of
        // extra text into the chat panel.
        if (cancel_flag_.load(std::memory_order_relaxed)) return;
        frontends_.broadcast([&](IFrontend& fe) { fe.on_token(token); });
        maybe_fire_progress(false);
    };
    cbs.on_reasoning_token = [&](const std::string& token) {
        mark_first_token();
        accumulated_reasoning += token;
        if (cancel_flag_.load(std::memory_order_relaxed)) return;
        frontends_.broadcast([&](IFrontend& fe) { fe.on_reasoning_token(token); });
        maybe_fire_progress(false);
    };
    cbs.on_tool_calls = [&](const std::vector<ToolCallRequest>& calls) {
        tool_call_requests = calls;
    };
    cbs.on_complete = [&]() {
        spdlog::trace("AgentLoop: LLM step complete, text={} chars, "
                      "reasoning={} chars, tool_calls={}",
                      accumulated_text.size(), accumulated_reasoning.size(),
                      tool_call_requests.size());
    };
    cbs.on_error = [&](const std::string& err) {
        out.had_error = true;
        frontends_.broadcast([&](IFrontend& fe) { fe.on_error(err); });
        activity_.emit(ActivityKind::error, "LLM error", err);
    };
    cbs.on_usage = [&](const CompletionUsage& u) {
        budget_.set_server_total(u.total_tokens);
        budget_.set_server_split(u.prompt_tokens, u.completion_tokens);
        reasoning_tokens_reported = u.reasoning_tokens;
        usage_reported = u;
        spdlog::trace("AgentLoop: server reports {} total tokens "
                      "(prompt={}, completion={}, reasoning={})",
                      u.total_tokens, u.prompt_tokens, u.completion_tokens,
                      u.reasoning_tokens);
    };
    cbs.on_should_cancel = [&]() {
        return cancel_flag_.load(std::memory_order_relaxed);
    };

    llm_.stream_completion(history.messages(), tool_schemas, cbs);

    auto stream_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - stream_t0).count();
    if (metrics_)
        metrics_->record_llm_step(usage_reported, stream_ms, prev_total_for_metrics);

    // S4.F diagnostic log per LLM POST. One line covers everything you need
    // to root-cause a cache miss: the system-prompt hash (byte-stable across
    // a session if the S4.F invariant holds), prompt_tokens, cached_tokens,
    // and time-to-first-token. When two consecutive log lines share the same
    // sys_hash and the second has cached_tokens > 0 (or a much lower ttft),
    // the prefix cache is working.
    {
        double prefill_per_token = (usage_reported.prompt_tokens > 0 && ttft_ms > 0)
            ? (static_cast<double>(ttft_ms) /
                 static_cast<double>(usage_reported.prompt_tokens))
            : 0.0;
        spdlog::info(
            "LLM POST: sys_hash={:016x} sys_chars={} prompt={} cached={} "
            "completion={} ttft_ms={} prefill_ms_per_prompt_token={:.2f}",
            sys_hash, sys_chars,
            usage_reported.prompt_tokens, usage_reported.cached_tokens,
            usage_reported.completion_tokens, ttft_ms, prefill_per_token);
    }

    // Final progress emission so the footer chip lands on the exact
    // streamed-character count rather than stopping at the last 150 ms tick.
    maybe_fire_progress(true);

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
