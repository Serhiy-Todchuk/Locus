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

// Exact string-match edit (Claude-Code-style: find `old_string` in the file,
// replace with `new_string`). Uniqueness check fails if `old_string` occurs
// more than once and `replace_all` is false — the LLM is expected to widen
// the match with extra surrounding context and retry. Requires a prior
// `read_file` on the same path in this process (S4.A).
class EditFileTool : public ITool {
public:
    std::string name()        const override { return "edit_file"; }
    std::string description() const override {
        return "Edit a file by exact string replacement. Prefer this over write_file — "
               "it is faster, cheaper, and immune to drift. `old_string` must match "
               "exactly (including whitespace) and must be unique in the file unless "
               "`replace_all` is true. You must have read the file earlier in this "
               "session before editing it.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path",        "string",  "Relative path from workspace root", true},
            {"old_string",  "string",  "Exact text to find. Must be unique unless replace_all=true.", true},
            {"new_string",  "string",  "Replacement text.",                  true},
            {"replace_all", "boolean", "Replace every occurrence (default false).", false},
        };
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

// Atomic batch of edits on a single file. Either every edit applies, or the
// file is left untouched. Each edit is applied in order to the running
// buffer; later edits see the results of earlier edits. Uniqueness is
// checked per edit just like `edit_file`.
class MultiEditFileTool : public ITool {
public:
    std::string name()        const override { return "multi_edit_file"; }
    std::string description() const override {
        return "Apply an atomic list of (old_string, new_string) edits to a single file. "
               "Edits apply in order; either all succeed or none are written. Use when "
               "one logical change needs several related replacements.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path",  "string", "Relative path from workspace root", true},
            {"edits", "array",
             "Array of edits. Each item: "
             "{\"old_string\": str, \"new_string\": str, \"replace_all\"?: bool}.", true},
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
