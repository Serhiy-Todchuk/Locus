#pragma once

#include "tool.h"

namespace locus {

class ReadFileTool : public ITool {
public:
    std::string name()        const override { return "read_file"; }
    std::string description() const override {
        return "Read the contents of a file. Use offset and length to paginate large files. "
               "Returns line-numbered content.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path",   "string",  "Relative path from workspace root", true},
            {"offset", "integer", "Line number to start reading from (1-based, default 1)", false},
            {"length", "integer", "Number of lines to read (default 100)", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

class WriteFileTool : public ITool {
public:
    std::string name()        const override { return "write_file"; }
    std::string description() const override {
        return "Write content to an existing file, replacing its contents entirely.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path",    "string", "Relative path from workspace root", true},
            {"content", "string", "The full content to write",         true},
        };
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

class CreateFileTool : public ITool {
public:
    std::string name()        const override { return "create_file"; }
    std::string description() const override {
        return "Create a new file at the given path. Fails if the file already exists.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path",    "string", "Relative path from workspace root", true},
            {"content", "string", "Initial file content",              true},
        };
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

class DeleteFileTool : public ITool {
public:
    std::string name()        const override { return "delete_file"; }
    std::string description() const override {
        return "Permanently delete a file from the workspace. This cannot be undone.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path", "string", "Relative path from workspace root", true},
        };
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

} // namespace locus
