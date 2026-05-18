#pragma once

#include "tool.h"

namespace locus {

// S4.D -- Plan-mode tools.
//
// `propose_plan` is the only tool the model can call while AgentMode==plan.
// It validates and echoes a structured plan; AgentCore observes the result,
// stores it, and notifies the frontend for the user's approve/reject.
//
// `mark_step_done` is only visible while AgentMode==execute. The model calls
// it as it works through the approved plan; AgentCore advances per-step
// status and emits on_plan_step_advanced / on_plan_completed events.

class ProposePlanTool : public ITool {
public:
    std::string name()        const override { return "propose_plan"; }
    std::string description() const override {
        return "Propose a numbered plan of steps for the user to review and "
               "approve. Use this exactly once when in plan mode. After the "
               "user approves, you will be put into execute mode with the "
               "full tool catalog. The user can also reject and re-prompt.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"title",   "string", "Short label for the plan (one line, ~60 chars).", false},
            {"summary", "string", "Optional one-paragraph rationale for the plan.", false},
            {"steps",   "array",
                "Ordered list of steps. Each step is an object with "
                "{description: string, tools_needed?: [string]}.", true},
        };
    }

    // Auto-approve -- the user approves the resulting plan via the chat
    // panel, not via the per-tool gate. The plan tool itself is harmless
    // (it just echoes structured JSON).
    ToolApprovalPolicy approval_policy() const override {
        return ToolApprovalPolicy::auto_approve;
    }

    // Visible only when the agent is in plan mode.
    bool visible_in_mode(ToolMode mode) const override {
        return mode == ToolMode::plan;
    }

    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

class MarkStepDoneTool : public ITool {
public:
    std::string name()        const override { return "mark_step_done"; }
    std::string description() const override {
        return "Mark a step in the active plan as done or failed. Call this "
               "exactly once per completed step, in order. `step` is the "
               "1-based index of the step you just finished.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"step",   "integer", "1-based step index just finished.", true},
            {"status", "string",  "\"done\" (default) or \"failed\".", false},
            {"notes",  "string",  "Optional one-line note shown to the user.", false},
        };
    }

    ToolApprovalPolicy approval_policy() const override {
        return ToolApprovalPolicy::auto_approve;
    }

    // Visible only in execute mode.
    bool visible_in_mode(ToolMode mode) const override {
        return mode == ToolMode::execute;
    }

    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus
