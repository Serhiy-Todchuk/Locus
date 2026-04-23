#pragma once

#include "tool.h"

namespace locus {

// S3.L: unified search face. Replaces the four individual search tools in the
// exposed manifest — dispatches to the same underlying index calls via an
// internal `mode` parameter. Registered by `register_builtin_tools()`.
class SearchTool : public ITool {
public:
    std::string name()        const override { return "search"; }
    std::string description() const override {
        return "Unified workspace search. `mode` selects the backend: "
               "text (FTS5 keyword, default), symbols (code definitions), "
               "semantic (vector similarity), hybrid (BM25 + vector). "
               "Semantic and hybrid require semantic search to be enabled.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"query",       "string",  "Search query. Keywords for text/hybrid, "
                                       "symbol name/prefix for symbols, "
                                       "natural language for semantic.", true},
            {"mode",        "string",  "One of: text, symbols, semantic, hybrid. "
                                       "Defaults to text.", false},
            {"max_results", "integer", "Maximum results (default 20 for text/symbols, "
                                       "10 for semantic/hybrid).", false},
            {"kind",        "string",  "symbols mode only: filter by kind "
                                       "(function, class, struct, method).", false},
            {"language",    "string",  "symbols mode only: filter by language.", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    ToolResult execute(const ToolCall& call, IWorkspaceServices& ws) override;
};

// The four individual search tools below remain available as direct `ITool`s
// (e.g. for slash-command testing) but are NOT registered by
// `register_builtin_tools()` since S3.L — the LLM sees a single `search` face.

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
