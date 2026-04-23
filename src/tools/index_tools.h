#pragma once

#include "tool.h"

namespace locus {

class ListDirectoryTool : public ITool {
public:
    std::string name()        const override { return "list_directory"; }
    std::string description() const override {
        return "List files and directories at a path using the workspace index. "
               "Returns names, sizes, and types.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path",  "string",  "Relative path from workspace root (empty or '.' for root)", false},
            {"depth", "integer", "Depth of listing: 0 = immediate children (default), 1+ = recurse", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

class GetFileOutlineTool : public ITool {
public:
    std::string name()        const override { return "get_file_outline"; }
    std::string description() const override {
        return "Get the structure of a file: headings and code symbols with line numbers. "
               "Does not read file content.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path", "string", "Relative path from workspace root", true},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

} // namespace locus
