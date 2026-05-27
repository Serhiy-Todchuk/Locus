#pragma once

#include "tool.h"

namespace locus {

class AskUserTool : public ITool {
public:
    std::string name()        const override { return "ask_user"; }
    std::string description() const override {
        return "Ask the user a question and wait for their response. "
               "Use this when you need clarification, confirmation, or input "
               "before proceeding. The user's response is returned as the result.\n"
               "Example:\n"
               "  ask_user({\"question\": \"Which test runner should I configure -- "
               "Catch2 or GoogleTest?\"})";
    }
    std::string short_description() const override {
        return "ask_user(question) -- prompt the user; returns their response.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"question", "string", "The question to ask the user", true},
        };
    }
    // Goes through the approval gate -- the frontend shows the question
    // and provides a response input. The user's answer is injected via
    // tool_decision(modify, {"question": "...", "response": "user answer"}).
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::ask; }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus
