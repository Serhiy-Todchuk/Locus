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
};

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

    ILLMClient&         llm_;
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
