#pragma once

#include "tool.h"

namespace locus {

// S6.17 Task G -- per-mode top-level tools (ADR-0008). The unified
// `SearchTool` dispatcher was retired here: a tool whose identity depends on
// a string discriminator (`mode`) inside the args is harder for an LLM to
// call correctly than N tools named after their actual purpose. Per-mode
// tools were already implemented as separate classes (this file's bottom
// half); they are now the direct registry entries. ADR-0009 later retired
// `search_hybrid` leaving five: text / regex / symbols / semantic / ast.
//
// Capability gating (S5.A):
//   semantic_search   off -> search_semantic hides via available()
//   code_aware_search off -> search_symbols / search_ast hide via available()
//
// Backward-compat shim for saved sessions lives in SessionManager::load /
// load_full (one milestone; removed next milestone).
//
// max_results defaults -- DO NOT change without re-doing the math:
//   text     = 8   -- top-N relevance; FTS5 snippet ~24 tokens (~100 chars).
//                    Total ~800 chars. Tightened in S5.N after WS2 (HTML
//                    corpus) showed raw-bytes snippets dumping ~1200 tok per
//                    call.
//   semantic = 5   -- top-N relevance; chunk snippet capped 250 chars
//   hybrid   = 5      ([search_tools.cpp] -- search of "k_snippet" / 250).
//                    Total ~1250 chars. Same S5.N reshape.
//   regex    = 50  -- EXHAUSTIVE: "all callers of foo", "every TODO(XXX)".
//   ast      = 50    Per-hit ~150-250 chars (path:line + 2-line snippet,
//   symbols  = 50    inline_match_text capped 200). Total ~10 KB / ~2500 tok.
//                    50 is the safety net, not a relevance trim. (symbols
//                    rows are tiny, but ambiguous names like "init" can
//                    return 100+ -- same cap applies as a guardrail.)

class SearchTextTool : public ITool {
public:
    std::string name()        const override { return "search_text"; }
    std::string description() const override {
        return "FTS5 keyword search across indexed text content. Indexed "
               "content is the EXTRACTED text from every file: source code "
               "as-is, Markdown / HTML stripped of markup, and PDF / DOCX / "
               "XLSX extracted via PDFium / OOXML parsers. Returns ranked "
               "results with snippets.\n"
               "Granularity: one row per file (FTS5 snippet of the strongest match).\n"
               "Example:\n"
               "  search_text({\"query\": \"WorkspaceConfig\"})";
    }
    std::string short_description() const override {
        return "search_text(query, max_results=8) -- FTS5 keyword search.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"query",       "string",  "Keyword search query (FTS5 syntax supported).", true},
            {"max_results", "integer", "Maximum results (default 8).", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

class SearchRegexTool : public ITool {
public:
    std::string name()        const override { return "search_regex"; }
    std::string description() const override {
        return "ECMAScript regex search over raw file content. Unlike "
               "`search_text`, preserves punctuation and case -- use for "
               "exact identifiers (`->m_cache`), operators, and TODO tags "
               "(`TODO(XXX)`).\n"
               "Granularity: one row per match (capped at `max_results`).\n"
               "Example:\n"
               "  search_regex({\"query\": \"->m_cache\"})";
    }
    std::string short_description() const override {
        return "search_regex(query, path_glob?, case_sensitive=true, max_results=50) -- ECMAScript regex per-line.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"query",          "string",  "ECMAScript regex pattern.", true},
            {"path_glob",      "string",  "Optional glob to limit which indexed files "
                                          "are searched (matched against relative path).", false},
            {"case_sensitive", "boolean", "Defaults to true.", false},
            {"max_results",    "integer", "Maximum matches to return (default 50).", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

class SearchSymbolsTool : public ITool {
public:
    std::string name()        const override { return "search_symbols"; }
    std::string description() const override {
        return "Look up code symbols (functions, classes, structs, methods) "
               "by name. Supports prefix matching. Takes `name`, not "
               "`query` (unlike the other search_* tools).\n"
               "Granularity: one row per symbol declaration.\n"
               "Example:\n"
               "  search_symbols({\"name\": \"ToolDispatcher\", \"kind\": \"class\"})";
    }
    std::string short_description() const override {
        return "search_symbols(name, kind?, language?, max_results=50) -- code-symbol lookup.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"name",        "string",  "Symbol name or prefix to search for.", true},
            {"kind",        "string",  "Filter by kind: function, class, struct, method (optional).", false},
            {"language",    "string",  "Filter by language (optional).", false},
            {"max_results", "integer", "Maximum symbols to return (default 50).", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    bool available(IWorkspaceServices& ws) const override;
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

class SearchSemanticTool : public ITool {
public:
    std::string name()        const override { return "search_semantic"; }
    std::string description() const override {
        return "Vector-cosine similarity search across indexed chunks. "
               "Finds conceptually related content even without exact "
               "keyword matches. Requires semantic search to be enabled.\n"
               "Granularity: one row per chunk (ranked by similarity).\n"
               "Example:\n"
               "  search_semantic({\"query\": \"how does the indexer batch file events\"})";
    }
    std::string short_description() const override {
        return "search_semantic(query, max_results=5) -- vector similarity.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"query",       "string",  "Natural language query describing what to find.", true},
            {"max_results", "integer", "Maximum results to return (default 5).", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    bool available(IWorkspaceServices& ws) const override;
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

// ADR-0009: `SearchHybridTool` retired. The class is gone from the header;
// `IndexQuery::search_hybrid` is still reachable from the retrieval-eval
// harness. No back-compat shim -- saved sessions that referenced
// `search_hybrid` fail at dispatch with an unknown-tool error.

// Tree-sitter structural search (S4.M). Compiles a `.scm` query against the
// chosen language's grammar and reports every match across the indexed files.
class SearchAstTool : public ITool {
public:
    std::string name()        const override { return "search_ast"; }
    std::string description() const override {
        return "Tree-sitter S-expression structural search. Finds shapes "
               "(e.g. all `malloc` calls, classes inheriting `ITool`, empty "
               "if-bodies) -- not just names. Requires `language` and a "
               "`query` written against that grammar.\n"
               "Granularity: one row per AST node match.\n"
               "Example:\n"
               "  search_ast({\"language\": \"cpp\", "
               "\"query\": \"(call_expression function: (identifier) @fn "
               "(#eq? @fn \\\"malloc\\\"))\"})";
    }
    std::string short_description() const override {
        return "search_ast(language, query, path_glob?, capture?, max_results=50) -- Tree-sitter S-expression queries.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"language",    "string",  "One of c, cpp, python, javascript, "
                                       "typescript, go, rust, java, csharp, "
                                       "ruby, php, bash, json, yaml, markdown, "
                                       "swift, kotlin.", true},
            {"query",       "string",  "Tree-sitter S-expression query. Example: "
                                       "`(call_expression function: (identifier) @fn "
                                       "(#eq? @fn \"malloc\"))`.", true},
            {"path_glob",   "string",  "Optional glob to limit indexed files "
                                       "(matched against relative path).", false},
            {"capture",     "string",  "Report only this @capture name "
                                       "(default: every capture).", false},
            {"max_results", "integer", "Maximum matches to return (default 50).", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    bool available(IWorkspaceServices& ws) const override;
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus
