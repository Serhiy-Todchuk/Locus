#pragma once

#include "conversation.h"
#include "core/workspace_services.h"
#include "../core/frontend_registry.h"
#include "llm/llm_client.h"
#include "../tools/tool.h"

#include <atomic>
#include <string>
#include <vector>

namespace locus {

class ActivityLog;
class ContextBudget;
class MetricsAggregator;

// Result of one LLM round trip. The orchestrator is responsible for
// appending `assistant_msg` to history and dispatching `tool_calls`.
struct AgentStepResult {
    ChatMessage           assistant_msg;
    std::vector<ToolCall> tool_calls;
    bool                  had_error = false;
    // Wall-clock duration of the LLM stream call that produced this step.
    // Paired with the round's completion_tokens via the context_meter
    // broadcast so the chat panel can show a per-bubble tok/s rate.
    long long             stream_ms = 0;

    // S6.13 -- reasoning watchdog telemetry. `tripped` = a threshold fired
    // during this round; `trigger` names which one ("chars"|"seconds").
    // `auto_nudge_fired` = the loop also cancelled the stream so the
    // AgentCore round handler should inject the steering message instead
    // of treating the cancel as a user-stop. `rounds_silent_at_trip` is
    // included for the activity event but is read from AgentCore's
    // turn-level counter, not from the loop.
    bool        watchdog_tripped       = false;
    bool        watchdog_auto_nudge    = false;
    std::string watchdog_trigger;

    // S6.21 Task 1 -- tool-call delivery accounting for this round, sourced
    // from the LMStudioClient's malformed-call gate (StreamCallbacks::
    // on_tool_calls_accounting). `delivered_tool_calls` is how many calls
    // passed the JSON-arguments gate; `dropped_tool_calls` is how many were
    // rejected as malformed. The all-dropped case (delivered=0, dropped>=1)
    // produces zero tool results, so the S6.10 repeated-call monitor and the
    // S6.20 build-error streak never fire -- AgentTurnRunner reads these to
    // nudge/abort that otherwise-silent loop. `dropped_diagnostic` carries the
    // compact per-call reason summary for the activity event.
    int         delivered_tool_calls   = 0;
    int         dropped_tool_calls     = 0;
    std::string dropped_diagnostic;
};

// S6.18 D.2 -- pure-function watchdog decision. Lifted out of run_step()'s
// in-line check_watchdog lambda so the silent-stream / text-only commit /
// reasoning-channel-heavy cases can be exercised without a mock LLM
// stream. Inputs are the bare state the closure was consulting; output is
// the trigger string ("" = no trip, "chars" | "seconds" = trip) AFTER the
// commit-phase and reasoning-channel-progress skips have been applied.
//
// `elapsed_seconds` is the time since stream start.
// `wd_max_chars` / `wd_max_seconds` are the configured budgets (0 = off).
// `recent_progress` is the most-recent timer-tick's observation of
// "delta > 0" -- maintained by the AgentLoop timer thread.
struct WatchdogDecisionInputs {
    int  text_chars        = 0;
    int  reasoning_chars   = 0;
    bool tool_call_seen    = false;
    long elapsed_seconds   = 0;
    int  wd_max_chars      = 0;
    int  wd_max_seconds    = 0;
    bool recent_progress   = true;
};

std::string evaluate_watchdog(const WatchdogDecisionInputs& in);

// Drives a single LLM call: builds the tool schema, streams tokens,
// accumulates reasoning/text/tool-call fragments, records an activity
// event, and returns the pieces that belong to the conversation. Does
// not know how tools are executed -- that's ToolDispatcher's job.
class AgentLoop {
public:
    AgentLoop(ILLMClient& llm,
              IToolRegistry& tools,
              IWorkspaceServices& services,
              ActivityLog& activity,
              ContextBudget& budget,
              FrontendRegistry& frontends,
              std::atomic<bool>& cancel_flag,
              MetricsAggregator* metrics = nullptr);

    // Run one LLM step. `tool_mode` controls which subset of the catalog is
    // exposed to the model -- agent (default), execute (S4.D plan running),
    // or plan (S4.D, only propose_plan visible).
    AgentStepResult run_step(const ConversationHistory& history,
                             ToolMode tool_mode = ToolMode::agent);

    // S6.16 -- reseat the LLM client (endpoint hot-swap). Called by AgentCore
    // on the agent thread between turns; the agent thread is the sole user of
    // the client so no locking is needed.
    void set_llm(ILLMClient& llm) { llm_ = &llm; }

private:
    // Builds the filtered per-turn tool manifest (S3.L) and logs its token
    // footprint. Returns the ToolSchema vector the LLM client consumes.
    //
    // The tool_manifest activity event is only emitted when the manifest
    // *content* changes vs. the last round (mode switch, MCP crash, MCP
    // restart, etc.). Per-round emission produced an entry between every
    // tool_call/tool_result pair even though the manifest was usually
    // identical; readers couldn't tell apart "manifest changed" from
    // "another round happened". The hash check kills the noise without
    // hiding actual changes -- a warning-level emit still fires on every
    // round when the manifest is over the budget threshold.
    std::vector<ToolSchema> build_tool_schemas(ToolMode mode);

    // Per-workspace threshold above which we warn about manifest bloat.
    int manifest_warn_tokens() const;

    ILLMClient*         llm_;   // reseatable (S6.16 endpoint hot-swap); never null
    IToolRegistry&      tools_;
    IWorkspaceServices& services_;
    ActivityLog&        activity_;
    ContextBudget&      budget_;
    FrontendRegistry&   frontends_;
    std::atomic<bool>&  cancel_flag_;
    MetricsAggregator*  metrics_ = nullptr;  // optional: nullptr disables recording

    // Hash of the most recently emitted tool_manifest activity content.
    // 0 = nothing emitted yet (forces emit on first round of the session).
    std::size_t         last_manifest_hash_ = 0;
};

} // namespace locus
