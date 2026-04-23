#pragma once

#include "conversation.h"
#include "core/workspace_services.h"
#include "frontend_registry.h"
#include "llm_client.h"
#include "tool.h"

#include <string>
#include <vector>

namespace locus {

class ActivityLog;
class ContextBudget;

// Result of one LLM round trip. The orchestrator is responsible for
// appending `assistant_msg` to history and dispatching `tool_calls`.
struct AgentStepResult {
    ChatMessage           assistant_msg;
    std::vector<ToolCall> tool_calls;
    bool                  had_error = false;
};

// Drives a single LLM call: builds the tool schema, streams tokens,
// accumulates reasoning/text/tool-call fragments, records an activity
// event, and returns the pieces that belong to the conversation. Does
// not know how tools are executed — that's ToolDispatcher's job.
class AgentLoop {
public:
    AgentLoop(ILLMClient& llm,
              IToolRegistry& tools,
              IWorkspaceServices& services,
              ActivityLog& activity,
              ContextBudget& budget,
              FrontendRegistry& frontends);

    AgentStepResult run_step(const ConversationHistory& history);

private:
    // Builds the filtered per-turn tool manifest (S3.L) and logs its token
    // footprint. Returns the ToolSchema vector the LLM client consumes.
    std::vector<ToolSchema> build_tool_schemas() const;

    // Per-workspace threshold above which we warn about manifest bloat.
    int manifest_warn_tokens() const;

    ILLMClient&         llm_;
    IToolRegistry&      tools_;
    IWorkspaceServices& services_;
    ActivityLog&        activity_;
    ContextBudget&      budget_;
    FrontendRegistry&   frontends_;
};

} // namespace locus
