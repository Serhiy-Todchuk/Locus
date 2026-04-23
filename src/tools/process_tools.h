#pragma once

#include "tool.h"

namespace locus {

class RunCommandTool : public ITool {
public:
    std::string name()        const override { return "run_command"; }
    std::string description() const override {
        return "Execute a shell command in the workspace directory. "
               "Returns stdout, stderr, and exit code.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"command",    "string",  "The command to execute",                        true},
            {"timeout_ms", "integer", "Timeout in milliseconds (default 30000)",       false},
        };
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

} // namespace locus
