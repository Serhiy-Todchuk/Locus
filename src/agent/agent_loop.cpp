#include "agent_loop.h"

#include "activity_log.h"
#include "context_budget.h"
#include "../core/frontend.h"
#include "llm/token_counter.h"
#include "metrics.h"
#include "../tools/tool_registry.h"
#include "../core/workspace.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace locus {

// S6.18 D.2 -- pure decision shared with the run_step closure and tests.
// Returns "" when no trip, "chars" / "seconds" when the named budget fires
// AFTER the C.2 (text-only commit) and D.1 (reasoning-channel-progress)
// skips have been applied. The seconds budget is evaluated independently of
// chars: when a chars trigger is skipped, the function falls through to
// re-check the seconds budget so a genuinely-stuck stream (long elapsed,
// large chars) still trips on seconds. C.2 / D.1 only suppress chars.
std::string evaluate_watchdog(const WatchdogDecisionInputs& in)
{
    constexpr int k_commit_text_floor = 100;
    int combined = in.text_chars + in.reasoning_chars;

    bool chars_trigger = (in.wd_max_chars > 0 && combined > in.wd_max_chars);
    bool seconds_trigger = (in.wd_max_seconds > 0
                            && in.elapsed_seconds > in.wd_max_seconds);

    // C.2 -- text-only commit-phase suppresses chars (S6.17). The model is
    // mid-final-answer; not stuck. Seconds still bites if it's actually idle.
    if (chars_trigger
        && !in.tool_call_seen
        && in.text_chars > k_commit_text_floor
        && in.reasoning_chars == 0) {
        chars_trigger = false;
    }

    // D.1 -- reasoning-channel-progress suppresses chars (S6.18). Productive
    // reasoning streams shouldn't burn a nudge strike on volume alone.
    if (chars_trigger
        && !in.tool_call_seen
        && in.reasoning_chars > 0
        && in.recent_progress) {
        chars_trigger = false;
    }

    if (chars_trigger) return "chars";
    if (seconds_trigger) return "seconds";
    return {};
}

AgentLoop::AgentLoop(ILLMClient& llm,
                     IToolRegistry& tools,
                     IWorkspaceServices& services,
                     ActivityLog& activity,
                     ContextBudget& budget,
                     FrontendRegistry& frontends,
                     std::atomic<bool>& cancel_flag,
                     MetricsAggregator* metrics)
    : llm_(&llm)
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
        return ws->config().agent.tool_manifest_warn_tokens;
    return 4000;  // default if no Workspace (tests, embedded use)
}

std::vector<ToolSchema> AgentLoop::build_tool_schemas(ToolMode mode)
{
    // S3.L: filter the manifest to tools that are available in this workspace
    // and visible in the current mode. S4.D introduced plan/execute modes.
    // S6.11 -- when `lazy_tool_manifest` is set, collapse parameter schemas to
    // permissive `additionalProperties:true`; the model fetches the real shape
    // via describe_tool when it needs to call.
    bool lazy = false;
    if (auto* ws = services_.workspace())
        lazy = ws->config().agent.lazy_tool_manifest;
    auto schema_json = tools_.build_schema_json(services_, mode, lazy);

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

    // S5.A -- summarise the active capability bucket combination so the log
    // line answers "why is this manifest this size?". Hyphen marks an off
    // bucket so the line is easy to grep.
    std::string buckets;
    if (auto* ws = services_.workspace()) {
        const auto& c = ws->config().capabilities;
        auto bit = [&](bool on, const char* short_name) {
            buckets += (on ? "+" : "-");
            buckets += short_name;
            buckets += " ";
        };
        bit(c.background_processes, "bg");
        bit(c.semantic_search,      "sem");
        bit(c.code_aware_search,    "code");
        bit(c.memory_bank,          "mem");
        bit(c.web_retrieval,        "web");
        if (!buckets.empty() && buckets.back() == ' ') buckets.pop_back();
    }
    if (over_budget) {
        spdlog::warn("Tool manifest: {} tools, ~{} tokens, {} bytes "
                     "(threshold {}; capabilities: {}). "
                     "Consider trimming or splitting by mode.",
                     schemas.size(), manifest_tokens,
                     schema_dump.size(), threshold, buckets);
    } else {
        spdlog::info("Tool manifest: {} tools, ~{} tokens, {} bytes "
                     "(capabilities: {})",
                     schemas.size(), manifest_tokens,
                     schema_dump.size(), buckets);
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
    // S6.17 Task C.1 -- shadow counters the timer thread can read without
    // racing on std::string::size() during appends. Updated alongside each
    // append in the chunk callbacks below.
    std::atomic<std::size_t> text_chars_atomic{0};
    std::atomic<std::size_t> reasoning_chars_atomic{0};
    std::atomic<bool>        tool_call_seen_atomic{false};
    std::vector<ToolCallRequest> tool_call_requests;
    int reasoning_tokens_reported = 0;
    CompletionUsage usage_reported{};
    int prev_total_for_metrics = budget_.prev_turn_total();
    auto stream_t0 = std::chrono::steady_clock::now();
    bool first_token_seen = false;
    long long ttft_ms = 0;

    // S6.13 -- reasoning watchdog. Triggers fire at most once per round;
    // `out.watchdog_tripped` survives onto AgentCore so the round-loop sees
    // it even after the stream callback returned.
    int  wd_max_seconds       = 0;
    int  wd_max_chars         = 0;
    bool wd_auto_nudge        = false;
    if (auto* ws = services_.workspace()) {
        wd_max_seconds = ws->config().agent.reasoning_max_seconds;
        wd_max_chars   = ws->config().agent.reasoning_max_chars;
        wd_auto_nudge  = ws->config().agent.reasoning_auto_nudge;
    }
    std::atomic<bool> watchdog_tripped_atomic{false};
    // S6.18 D.1 -- "progress observed on the most recent timer tick" flag.
    // Maintained by the timer thread only: at every tick it reads the
    // current text+reasoning total, compares against its private prev
    // reading, and sets this flag to (delta > 0). evaluate_watchdog()
    // consults the flag to suppress chars-budget trips during productive
    // reasoning-channel-heavy streams. Putting the delta computation on
    // the timer thread keeps the per-tick semantics clean -- chunk
    // callbacks fire too frequently to participate in delta tracking
    // themselves.
    std::atomic<bool> wd_recent_progress_atomic{true};  // assume progress at t=0
    std::atomic<bool> wd_skip_logged_atomic{false};     // S6.18 D.2 diag throttle
    auto check_watchdog = [&]() {
        if (watchdog_tripped_atomic.load(std::memory_order_acquire)) return;
        // S6.17 Task C.1 -- read atomics so this is safe to call from the
        // timer thread alongside the chunk callbacks.
        WatchdogDecisionInputs in;
        in.text_chars      = static_cast<int>(text_chars_atomic.load());
        in.reasoning_chars = static_cast<int>(reasoning_chars_atomic.load());
        in.tool_call_seen  = tool_call_seen_atomic.load(std::memory_order_acquire);
        in.elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - stream_t0).count();
        in.wd_max_chars    = wd_max_chars;
        in.wd_max_seconds  = wd_max_seconds;
        in.recent_progress = wd_recent_progress_atomic.load(std::memory_order_acquire);

        std::string trigger = evaluate_watchdog(in);
        if (trigger.empty()) {
            // S6.18 D.2 -- per-skip diagnostic. evaluate_watchdog already
            // applied the C.2 / D.1 skip rules; mirror them here so the log
            // tells the user WHY the watchdog stayed silent during a stream
            // that nominally exceeded the chars budget. The throttle is the
            // single-shot `wd_skip_logged_atomic` -- one info line per stream
            // is enough.
            bool combined_over = (in.wd_max_chars > 0
                                  && (in.text_chars + in.reasoning_chars)
                                     > in.wd_max_chars);
            if (combined_over && !wd_skip_logged_atomic.exchange(true)) {
                if (!in.tool_call_seen
                    && in.text_chars > 100
                    && in.reasoning_chars == 0) {
                    spdlog::info("AgentLoop: watchdog tick during commit phase "
                                 "(text_chars={}, reasoning_chars=0, no tool_calls); "
                                 "suppressed chars-budget trip",
                                 in.text_chars);
                } else if (!in.tool_call_seen
                           && in.reasoning_chars > 0
                           && in.recent_progress) {
                    spdlog::info("AgentLoop: watchdog tick during productive "
                                 "reasoning (reasoning_chars={}, text_chars={}); "
                                 "suppressed chars-budget trip",
                                 in.reasoning_chars, in.text_chars);
                }
            }
            return;
        }

        // CAS to single-trip; both chunk callbacks AND the timer thread can
        // race here.
        bool expected = false;
        if (!watchdog_tripped_atomic.compare_exchange_strong(expected, true))
            return;

        int combined = in.text_chars + in.reasoning_chars;
        out.watchdog_tripped = true;
        out.watchdog_trigger = trigger;
        spdlog::warn("AgentLoop: reasoning watchdog tripped (trigger={}, "
                     "chars={}, auto_nudge={})",
                     trigger, combined, wd_auto_nudge);
        if (wd_auto_nudge) {
            out.watchdog_auto_nudge = true;
            // Break the stream cleanly via the existing cancel pathway. The
            // round-loop in AgentCore treats step.watchdog_auto_nudge as a
            // soft-cancel and re-enters with an injected steering message.
            cancel_flag_.store(true, std::memory_order_release);
        }
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_reasoning_watchdog_tripped(trigger, combined);
        });
    };

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

    // S5.D -- refuse the LLM round when the prompt estimate already eats into
    // the response reserve. The user must compact before the turn can proceed.
    // We check here (after tool schemas are built) so the reserve-breach
    // message names the actual manifest cost, not a stale estimate.
    {
        int limit   = budget_.limit();
        int reserve = budget_.reserve();
        if (limit > 0 && reserve > 0) {
            int prompt_est = history.estimate_tokens();
            if (prompt_est + reserve > limit) {
                spdlog::warn("AgentLoop: reserve breach ({} prompt + {} reserve > {} limit)",
                             prompt_est, reserve, limit);
                std::string msg =
                    "Context too full to generate a useful response. "
                    "Prompt ~" + std::to_string(prompt_est) +
                    " tokens + " + std::to_string(reserve) +
                    " reserved headroom exceeds the " + std::to_string(limit) +
                    " token context window. "
                    "Use /compact (or the Compact button) to free space, "
                    "then resend your message.";
                activity_.emit(ActivityKind::warning,
                               "Reserve breach -- compact required",
                               msg);
                frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_error(msg);
                    fe.on_compaction_needed(prompt_est, limit);
                });
                out.had_error = true;
                return out;
            }
        }
    }

    StreamCallbacks cbs;
    cbs.on_token = [&](const std::string& token) {
        mark_first_token();
        accumulated_text += token;
        text_chars_atomic.store(accumulated_text.size(),
                                std::memory_order_release);
        // S6.13 -- a model that streams a long plain-text answer without
        // ever calling a tool can also trip the watchdog. The trigger sees
        // total stream chars (reasoning + text), not reasoning-only.
        check_watchdog();
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
        reasoning_chars_atomic.store(accumulated_reasoning.size(),
                                     std::memory_order_release);
        // S6.13 -- check watchdog before propagating. If auto-nudge fires,
        // cancel_flag_ goes hot here so the next chunk in the stream tail
        // is suppressed below.
        check_watchdog();
        if (cancel_flag_.load(std::memory_order_relaxed)) return;
        frontends_.broadcast([&](IFrontend& fe) { fe.on_reasoning_token(token); });
        maybe_fire_progress(false);
    };
    cbs.on_tool_calls = [&](const std::vector<ToolCallRequest>& calls) {
        tool_call_requests = calls;
        if (!calls.empty())
            tool_call_seen_atomic.store(true, std::memory_order_release);
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

    // S6.10 Task C -- build the outgoing messages vector, optionally stripping
    // reasoning_content from past assistant messages. Strip every assistant
    // message EXCEPT the most recent if and only if the very last message in
    // history is a tool result (we're mid-decision-chain and the model may
    // still need its own reasoning to interpret the result). Assistant
    // messages that have ONLY reasoning (no content, no tool_calls) are
    // preserved intact -- those are vanishingly rare post-S6.13 and the
    // simple rule beats a sentence-counting heuristic.
    std::vector<ChatMessage> outgoing = history.messages();
    bool strip_enabled = true;
    if (auto* ws = services_.workspace())
        strip_enabled = ws->config().agent.strip_past_thinking;
    if (strip_enabled) {
        // Find the most recent assistant message.
        int last_assistant_idx = -1;
        for (int i = static_cast<int>(outgoing.size()) - 1; i >= 0; --i) {
            if (outgoing[i].role == MessageRole::assistant) {
                last_assistant_idx = i;
                break;
            }
        }
        bool keep_latest =
            !outgoing.empty() &&
            outgoing.back().role == MessageRole::tool;
        long long stripped_bytes = 0;
        for (int i = 0; i < static_cast<int>(outgoing.size()); ++i) {
            ChatMessage& m = outgoing[i];
            if (m.role != MessageRole::assistant) continue;
            if (m.reasoning_content.empty()) continue;
            // Preserve assistant messages with ONLY reasoning intact -- a one-
            // line check covers the post-S6.13 edge case without further
            // heuristics.
            if (m.content.empty() && m.tool_calls.empty()) continue;
            if (i == last_assistant_idx && keep_latest) continue;
            stripped_bytes += static_cast<long long>(m.reasoning_content.size());
            m.reasoning_content.clear();
        }
        if (stripped_bytes > 0) {
            spdlog::trace("AgentLoop: stripped {} bytes of past-turn reasoning "
                          "from outgoing payload",
                          stripped_bytes);
        }
    }
    // S6.17 Task C.1 -- timer thread that ticks check_watchdog() every 5s
    // independent of chunk arrival. Without this, a 200+ s silent stall
    // between chunks never trips the watchdog because the chunk callbacks
    // are the only place check_watchdog() ran pre-S6.17. The thread stops
    // as soon as stream_completion returns (the stop_flag wakes the wait).
    std::atomic<bool> watchdog_timer_stop{false};
    std::condition_variable watchdog_cv;
    std::mutex              watchdog_mu;
    std::thread watchdog_timer;
    if (wd_max_seconds > 0 || wd_max_chars > 0) {
        watchdog_timer = std::thread([&] {
            std::unique_lock lk(watchdog_mu);
            // S6.18 D.1 -- per-tick total tracking. Lives on the timer
            // thread so the delta is "progress across the last 5s tick"
            // rather than "since the last chunk arrival".
            std::size_t prev_total = 0;
            while (!watchdog_timer_stop.load(std::memory_order_acquire)) {
                if (watchdog_cv.wait_for(lk, std::chrono::seconds(5),
                        [&] {
                            return watchdog_timer_stop.load(
                                std::memory_order_acquire);
                        }))
                    break;
                std::size_t cur_total =
                    text_chars_atomic.load(std::memory_order_acquire)
                    + reasoning_chars_atomic.load(std::memory_order_acquire);
                bool progress = cur_total > prev_total;
                prev_total = cur_total;
                wd_recent_progress_atomic.store(progress,
                    std::memory_order_release);
                check_watchdog();
            }
        });
    }

    llm_->stream_completion(outgoing, tool_schemas, cbs);

    // Stop the timer thread before reading post-stream state.
    if (watchdog_timer.joinable()) {
        {
            std::lock_guard lk(watchdog_mu);
            watchdog_timer_stop.store(true, std::memory_order_release);
        }
        watchdog_cv.notify_all();
        watchdog_timer.join();
    }

    auto stream_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - stream_t0).count();
    if (metrics_)
        metrics_->record_llm_step(usage_reported, stream_ms, prev_total_for_metrics);
    out.stream_ms = stream_ms;

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
            detail += "[response]\n";
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
    // S6.10 Task C -- capture this turn's reasoning into the assistant
    // message so the next round (mid-decision-chain after a tool result)
    // sees the model's own thinking. AgentLoop's payload-prep strips it
    // again from older assistant messages so prefix cache survives.
    out.assistant_msg.reasoning_content = accumulated_reasoning;
    out.assistant_msg.tool_calls = tool_call_requests;

    out.tool_calls.reserve(tool_call_requests.size());
    for (auto& tc_req : tool_call_requests) {
        out.tool_calls.push_back(
            ToolRegistry::parse_tool_call(tc_req.id, tc_req.name, tc_req.arguments));
    }

    return out;
}

} // namespace locus
