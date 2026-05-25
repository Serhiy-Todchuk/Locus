#pragma once

#include "tool.h"

namespace locus {

// S5.Z task 8 -- regex/substring filter over arbitrary text the caller passes
// in as the `text` arg. Pure-CPU + no FS access -> auto-approve, category
// "read". The intended use is "filter the previous build output for 'error'"
// without burning context on the full output: the LLM emits a short regex and
// gets back only the matching lines (with optional N lines of leading /
// trailing context, like grep -A / -B).
class FilterOutputTool : public ITool {
public:
    std::string name()        const override { return "filter_output"; }
    std::string description() const override {
        return "Filter arbitrary text by regex or substring. Use to extract "
               "matching lines from a previous tool result (e.g. build log, "
               "test output) without sending the whole blob into context. "
               "Pass the text you want to filter as `text`; pattern as `pattern`. "
               "`mode` is `regex` (ECMAScript, default) or `substring`. "
               "`before` and `after` add N lines of context around each match "
               "(grep -B / -A). `invert` returns non-matching lines. The "
               "response is capped at `max_matches` lines (default 50).";
    }
    std::string short_description() const override {
        return "Filter text by regex/substring (grep-like, with context).";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"text",          "string",  "The text to filter (typically the "
                                          "content of a prior tool_result).", true},
            {"pattern",       "string",  "Search pattern. ECMAScript regex when "
                                          "mode=regex (default); literal substring "
                                          "when mode=substring.", true},
            {"mode",          "string",  "`regex` (default) or `substring`.", false},
            {"case_sensitive","boolean", "Default false.", false},
            {"max_matches",   "integer", "Maximum matching lines to return "
                                          "(default 50). Caps the response so a "
                                          "permissive pattern can't blow the "
                                          "context budget.", false},
            {"before",        "integer", "Include N lines of context BEFORE each "
                                          "match (default 0; max 10).", false},
            {"after",         "integer", "Include N lines of context AFTER each "
                                          "match (default 0; max 10).", false},
            {"invert",        "boolean", "When true, return lines that do NOT "
                                          "match the pattern (grep -v).", false},
        };
    }
    ToolApprovalPolicy approval_policy() const override {
        return ToolApprovalPolicy::auto_approve;
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult execute(const ToolCall& call, IWorkspaceServices& ws,
                       const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus
