#pragma once

#include "tool.h"

namespace locus {

class ListDirectoryTool : public ITool {
public:
    std::string name()        const override { return "list_directory"; }
    std::string description() const override {
        return "List files and directories at a path. Enumerates the real filesystem "
               "and annotates each entry: [<language>] for indexed text, [binary] for "
               "binary files (by extension or detected content), [oversized] for files "
               "above the indexer's size cap, [unindexed] for entries the index didn't "
               "track (try read_file if you suspect they are text). Workspace excludes "
               "(.git, node_modules, build, .locus, ...) are still hidden. Capped at "
               "max_entries (default 200) -- refine the path or use search for large dirs.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"path",        "string",  "Relative path from workspace root (empty or '.' for root)", false},
            {"depth",       "integer", "Depth of listing: 0 = immediate children (default), 1+ = recurse", false},
            {"max_entries", "integer", "Cap on entries returned (default 200; remainder reported as truncated)", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
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
    // S5.A -- hidden from the manifest when the code-aware-search capability
    // is off (the outline is essentially the symbols view of a single file).
    bool        available(IWorkspaceServices& ws) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus
