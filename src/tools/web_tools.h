#pragma once

#include "tool.h"

namespace locus {

// S6.1 -- Web Retrieval (RAG) tools.
//
// All three gate on `capabilities.web_retrieval` via available(), so a
// workspace that hasn't opted in never sees them in the manifest. web_search /
// web_fetch require approval (the user sees the query / URL before anything
// leaves the machine -- the real control per the threat model); web_read is
// auto-approved (it only reads already-fetched, already-scanned local cache).
//
// The "index, don't inject" principle (architecture/web-retrieval.md): fetched
// pages NEVER enter the LLM context whole. web_fetch returns a ~30-token
// outline; the agent then pulls specific sections via web_read or finds terms
// via search_text(sources="web"). Untrusted content is injection-scanned and
// origin-stamped on the way into the cache (S6.0).

// Search the web via the configured provider. Approval: ask.
class WebSearchTool : public ITool {
public:
    std::string name()        const override { return "web_search"; }
    std::string description() const override {
        return "Search the web via the configured search provider (Brave by "
               "default). Returns a compact list of {title, url, snippet} -- "
               "NOT page contents. To read a result, follow up with web_fetch "
               "on the URL.\n"
               "Example:\n"
               "  web_search({\"query\": \"sqlite fts5 bm25 ranking\"})";
    }
    std::string short_description() const override {
        return "web_search(query, max_results=5) -- web search; returns titles + URLs + snippets.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"query",       "string",  "Search query.", true},
            {"max_results", "integer", "Maximum results (default from config, "
                                       "typically 5).", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::ask; }
    bool available(IWorkspaceServices& ws) const override;
    std::string preview(const ToolCall& call) const override;
    ToolResult execute(const ToolCall& call, IWorkspaceServices& ws,
                       const std::atomic<bool>* cancel_flag = nullptr) override;
};

// Fetch + index a single web page. Approval: ask. Returns the outline only;
// full content is indexed in the ephemeral web cache, never returned whole.
class WebFetchTool : public ITool {
public:
    std::string name()        const override { return "web_fetch"; }
    std::string description() const override {
        return "Fetch a web page over HTTP(S), strip it to readable text, and "
               "index it locally for searching. Returns the page OUTLINE "
               "(title, word count, headings) -- not the full text, which "
               "would blow the context budget. After fetching, read sections "
               "with web_read or find terms with search_text using "
               "sources=\"web\".\n"
               "Example:\n"
               "  web_fetch({\"url\": \"https://sqlite.org/fts5.html\"})";
    }
    std::string short_description() const override {
        return "web_fetch(url) -- fetch + index a page; returns its outline.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"url", "string", "Absolute http(s) URL to fetch.", true},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::ask; }
    bool available(IWorkspaceServices& ws) const override;
    std::string preview(const ToolCall& call) const override;
    ToolResult execute(const ToolCall& call, IWorkspaceServices& ws,
                       const std::atomic<bool>* cancel_flag = nullptr) override;
};

// Read a section of a previously-fetched page. Approval: auto (local cache,
// already scanned).
class WebReadTool : public ITool {
public:
    std::string name()        const override { return "web_read"; }
    std::string description() const override {
        return "Read a section of a previously fetched web page (like read_file "
               "but for web pages). Either pass a `heading` to read the section "
               "under it, or `offset` + `length` for a line window. The URL "
               "must have been web_fetch'd this session.\n"
               "Example:\n"
               "  web_read({\"url\": \"https://sqlite.org/fts5.html\", "
               "\"heading\": \"The bm25() function\"})";
    }
    std::string short_description() const override {
        return "web_read(url, heading? | offset?, length?) -- read a section of a fetched page.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"url",     "string",  "URL of a previously fetched page.", true},
            {"heading", "string",  "Read the section under this heading "
                                   "(case-insensitive prefix match).", false},
            {"offset",  "integer", "1-based line offset (default 1). Ignored "
                                   "when `heading` is given.", false},
            {"length",  "integer", "Number of lines to return (default 100).", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    bool available(IWorkspaceServices& ws) const override;
    std::string preview(const ToolCall& call) const override;
    ToolResult execute(const ToolCall& call, IWorkspaceServices& ws,
                       const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus
