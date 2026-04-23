#pragma once

#include "tool.h"

namespace locus {

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
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
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
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

class SearchSemanticTool : public ITool {
public:
    std::string name()        const override { return "search_semantic"; }
    std::string description() const override {
        return "Semantic search across all indexed files using vector similarity. "
               "Finds conceptually related content even without exact keyword matches. "
               "Requires semantic search to be enabled.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"query",       "string",  "Natural language query describing what to find", true},
            {"max_results", "integer", "Maximum results to return (default 10)", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

class SearchHybridTool : public ITool {
public:
    std::string name()        const override { return "search_hybrid"; }
    std::string description() const override {
        return "Hybrid search combining keyword (BM25) and semantic (vector) search. "
               "Best for queries that benefit from both exact matches and conceptual similarity. "
               "Requires semantic search to be enabled.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"query",       "string",  "Search query (supports both keywords and natural language)", true},
            {"max_results", "integer", "Maximum results to return (default 10)", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

} // namespace locus
