#pragma once

#include "tool.h"

namespace locus {

// -- Read-only tools (approval: auto) ----------------------------------------

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
    std::string approval_policy() const override { return "auto"; }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
};

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
    std::string approval_policy() const override { return "auto"; }
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
};

class SearchTextTool : public ITool {
public:
    std::string name()        const override { return "search_text"; }
    std::string description() const override {
        return "Full-text search across all indexed files. Returns ranked results with snippets.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"query",       "string",  "Search query (FTS5 syntax supported)", true},
            {"max_results", "integer", "Maximum results to return (default 20)", false},
        };
    }
    std::string approval_policy() const override { return "auto"; }
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
};

class SearchSymbolsTool : public ITool {
public:
    std::string name()        const override { return "search_symbols"; }
    std::string description() const override {
        return "Search for code symbols (functions, classes, structs, methods) by name. "
               "Supports prefix matching.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"name",     "string", "Symbol name or prefix to search for", true},
            {"kind",     "string", "Filter by kind: function, class, struct, method (optional)", false},
            {"language", "string", "Filter by language (optional)", false},
        };
    }
    std::string approval_policy() const override { return "auto"; }
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
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
    std::string approval_policy() const override { return "auto"; }
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
};

// -- Mutating tools (approval: always) ---------------------------------------

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
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
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
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
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
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
};

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
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
};

// -- Interactive tools -------------------------------------------------------

class AskUserTool : public ITool {
public:
    std::string name()        const override { return "ask_user"; }
    std::string description() const override {
        return "Ask the user a question and wait for their response. "
               "Use this when you need clarification, confirmation, or input "
               "before proceeding. The user's response is returned as the result.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"question", "string", "The question to ask the user", true},
        };
    }
    // Goes through the approval gate — the frontend shows the question
    // and provides a response input. The user's answer is injected via
    // tool_decision(modify, {"question": "...", "response": "user answer"}).
    std::string approval_policy() const override { return "always"; }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, const WorkspaceContext& ws) override;
};

// -- Factory: populate a registry with all built-in tools --------------------

void register_builtin_tools(IToolRegistry& registry);

} // namespace locus
