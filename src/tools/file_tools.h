#pragma once

#include "tool.h"

namespace locus {

class ReadFileTool : public ITool {
public:
    std::string name()        const override { return "read_file"; }
    std::string description() const override {
        return "Read the contents of a text file. Use offset and length to "
               "paginate large files. Returns line-numbered content. "
               "For binary document formats (PDF, DOCX, XLSX) read_file "
               "returns raw bytes -- use `search` instead: those formats "
               "are extracted to clean text at index time and are fully "
               "searchable. `get_file_outline` also works on them.\n"
               "Example:\n"
               "  read_file({\"path\": \"src/main.cpp\", \"offset\": 1, \"length\": 100})";
    }
    std::string short_description() const override {
        return "Read a line range of a text file (paginates large files).";
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
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

// Create a new file, or overwrite an existing one when `overwrite=true` is
// set explicitly. Parent directories are created as needed. Without
// `overwrite`, the tool fails if the target already exists -- protects
// against hallucinated paths that happen to collide with existing files.
// For modifying existing files, prefer `edit_file`.
class WriteFileTool : public ITool {
public:
    std::string name()        const override { return "write_file"; }
    std::string description() const override {
        return "Create a new file at the given path. Any missing parent directories "
               "are created automatically -- there is no separate mkdir / "
               "create_directory tool, write_file is sufficient for scaffolding a "
               "fresh project tree. Fails if the file already exists unless "
               "`overwrite=true` is set -- set that only when you intend to replace "
               "the whole file. For modifying existing files, prefer `edit_file`.\n"
               "Example:\n"
               "  write_file({\"path\": \"src/new_module.cpp\", \"content\": \"...\"})\n"
               "  // overwrite=false (default) refuses if the file exists.";
    }
    std::string short_description() const override {
        return "Create a new file (set overwrite=true to replace existing).";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path",      "string",  "Relative path from workspace root", true},
            {"content",   "string",  "The full content to write",         true},
            {"overwrite", "boolean",
             "Allow replacing an existing file (default false). Set only for full rewrites; "
             "prefer `edit_file` for partial changes.", false},
        };
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

// Exact string-match edit (Claude-Code-style: find `old_string` in the file,
// replace with `new_string`). Always takes an atomic list of edits -- pass a
// one-element array for the common single-edit case. Either every edit
// applies, or the file is left untouched. Each edit is applied in order to
// the running buffer; later edits see the results of earlier edits.
// Uniqueness fails when `old_string` occurs more than once and `replace_all`
// is false -- the LLM is expected to widen the match with surrounding context
// and retry. Requires a prior `read_file` on the same path in this process
// (S4.A).
class EditFileTool : public ITool {
public:
    std::string name()        const override { return "edit_file"; }
    std::string description() const override {
        return "Edit a file by exact string replacement. Prefer this over write_file -- "
               "it is faster, cheaper, and immune to drift. Pass an `edits` array "
               "(use a single element for one change, multiple for an atomic batch). "
               "Each `old_string` must match byte-for-byte (including whitespace) and "
               "must be unique in the file unless `replace_all` is true. Edits apply in "
               "order and either all succeed or none are written. "
               "PRECONDITION: read_file must have been called on the same path during "
               "this session. Call read_file first if you have not already -- "
               "edit_file will return an error otherwise. This guard catches blind "
               "edits against stale assumptions about file contents.\n"
               "Examples:\n"
               "  // Single edit:\n"
               "  edit_file({\"path\": \"src/foo.cpp\", \"edits\": [\n"
               "    {\"old_string\": \"int total = 0;\", \"new_string\": \"int total = 1;\"}\n"
               "  ]})\n"
               "  // Atomic batch of two:\n"
               "  edit_file({\"path\": \"src/foo.cpp\", \"edits\": [\n"
               "    {\"old_string\": \"oldFn(\", \"new_string\": \"newFn(\", \"replace_all\": true},\n"
               "    {\"old_string\": \"// TODO\", \"new_string\": \"// DONE\"}\n"
               "  ]})";
    }
    std::string short_description() const override {
        return "Apply exact-string edits to a file atomically (read_file first).";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path",  "string", "Relative path from workspace root", true},
            {"edits", "array",
             "Ordered list of replacements. Each item: "
             "{\"old_string\": str, \"new_string\": str, \"replace_all\"?: bool}.", true},
        };
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

class DeleteFileTool : public ITool {
public:
    std::string name()        const override { return "delete_file"; }
    std::string description() const override {
        return "Permanently delete a file from the workspace. This cannot be undone.";
    }
    std::string short_description() const override {
        return "Permanently delete a file from the workspace.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path", "string", "Relative path from workspace root", true},
        };
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus
