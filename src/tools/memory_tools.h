#pragma once

#include "tool.h"

namespace locus {

// S4.R memory bank tools. Both `available()=false` when the workspace has no
// MemoryStore (config disabled or workspace doesn't expose one); the tools
// then drop out of the per-turn manifest automatically.

// Creates a new memory entry. Approval policy: ask -- memories are durable,
// the user sees what's being committed before it lands.
class AddMemoryTool : public ITool {
public:
    std::string name()        const override { return "add_memory"; }
    std::string description() const override {
        return "Save a small verbatim note to the workspace memory bank. "
               "Use for facts you discover that should survive into future "
               "sessions (build commands, coding conventions, "
               "user preferences, gotchas). Keep entries short -- one to a "
               "few sentences. Tag liberally; the tags are searchable.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"content", "string", "The verbatim text to save. Will be stored "
                                  "exactly as written; no summarisation.", true},
            {"tags",    "array",  "Optional list of short tag strings.", false},
            {"pinned",  "boolean", "When true, the entry is always loaded into "
                                   "the system prompt (subject to budget) and "
                                   "survives GC unconditionally. Defaults to false.", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::ask; }
    bool available(IWorkspaceServices& ws) const override;
    ToolResult execute(const ToolCall& call, IWorkspaceServices& ws,
                       const std::atomic<bool>* cancel_flag = nullptr) override;
};

// Hybrid retrieval over the workspace memory bank. Approval: auto.
class SearchMemoryTool : public ITool {
public:
    std::string name()        const override { return "search_memory"; }
    std::string description() const override {
        return "Search the workspace memory bank for entries matching a "
               "query. Combines keyword (BM25), semantic (vector), and a "
               "recency boost. Returns the verbatim entries plus tags. Use "
               "when you suspect something useful was learned in a prior "
               "session and you want it back in context.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"query",       "string",  "Natural-language query or keywords.", true},
            {"max_results", "integer", "Maximum number of entries to return "
                                       "(default 5).", false},
            {"tags",        "array",   "Optional tag filter -- only entries "
                                       "carrying every listed tag are returned.", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    bool available(IWorkspaceServices& ws) const override;
    ToolResult execute(const ToolCall& call, IWorkspaceServices& ws,
                       const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus
