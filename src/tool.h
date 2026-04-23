#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace locus {

class IWorkspaceServices;

// -- Tool approval policy ---------------------------------------------------

enum class ToolApprovalPolicy {
    ask,           // "Ask Every Time" — show approval UI, require user decision
    auto_approve,  // "Auto-Approve"   — execute immediately, no user prompt
    deny           // "Deny"           — never execute, report rejection to the LLM
};

inline const char* to_string(ToolApprovalPolicy p) {
    switch (p) {
        case ToolApprovalPolicy::ask:          return "ask";
        case ToolApprovalPolicy::auto_approve: return "auto";
        case ToolApprovalPolicy::deny:         return "deny";
    }
    return "ask";
}

inline ToolApprovalPolicy policy_from_string(const std::string& s) {
    if (s == "auto")  return ToolApprovalPolicy::auto_approve;
    if (s == "deny")  return ToolApprovalPolicy::deny;
    return ToolApprovalPolicy::ask;  // default / "ask" / unknown
}

inline const char* policy_display_name(ToolApprovalPolicy p) {
    switch (p) {
        case ToolApprovalPolicy::ask:          return "Ask Every Time";
        case ToolApprovalPolicy::auto_approve: return "Auto-Approve";
        case ToolApprovalPolicy::deny:         return "Deny";
    }
    return "Ask Every Time";
}

// -- Tool visibility context ------------------------------------------------

// Context in which tool schemas are assembled. Individual tools opt in via
// ITool::visible_in_mode(). Defaults keep today's behavior: every tool is
// visible in `agent` mode and hidden from `plan` / `subagent` (S3.L — the
// plan/subagent modes themselves land in M4, but the plumbing ships now).
enum class ToolMode {
    agent,     // normal interactive turn (default)
    plan,      // planning pass — execute-side tools (write, delete, run) hidden
    subagent   // delegated subtask — role-specific subset
};

// -- Structs passed to/from tools -------------------------------------------

struct ToolParam {
    std::string name;
    std::string type;         // "string" | "integer" | "boolean" | "array" | "object"
    std::string description;  // shown to LLM in schema
    bool        required = true;
};

struct ToolResult {
    bool        success;
    std::string content;      // injected into LLM context (compact, token-efficient)
    std::string display;      // shown to user in UI (may be richer / formatted)
};

struct ToolCall {
    std::string    id;        // from LLM (e.g. "call_abc123")
    std::string    tool_name;
    nlohmann::json args;
};

// -- ITool interface --------------------------------------------------------

class ITool {
public:
    virtual ~ITool() = default;

    virtual std::string              name()        const = 0;
    virtual std::string              description() const = 0;
    virtual std::vector<ToolParam>   params()      const = 0;

    virtual ToolResult execute(const ToolCall& call,
                               IWorkspaceServices& ws) = 0;

    // Default approval policy for this tool. User-configured overrides in
    // WorkspaceConfig take precedence — see AgentCore::resolve_policy().
    virtual ToolApprovalPolicy approval_policy() const { return ToolApprovalPolicy::ask; }

    // Human-readable preview of what this call will do (shown in approval UI)
    virtual std::string preview(const ToolCall& call) const { return ""; }

    // -- Context gating (S3.L) -----------------------------------------------
    // Whether this tool can run against the given workspace right now. Default
    // is always-available; override to hide tools whose substrate is missing
    // (e.g. git tools when no `.git/`, LSP tools when no server attached).
    // Filtered out of the per-turn manifest when false.
    virtual bool available(IWorkspaceServices& /*ws*/) const { return true; }

    // Whether this tool is exposed to the LLM in the given mode. Default
    // `agent` only — execute-side tools stay hidden from plan/subagent modes
    // unless they opt in.
    virtual bool visible_in_mode(ToolMode mode) const { return mode == ToolMode::agent; }
};

// -- IToolRegistry interface ------------------------------------------------

class IToolRegistry {
public:
    virtual ~IToolRegistry() = default;

    virtual void   register_tool(std::unique_ptr<ITool> tool) = 0;
    virtual ITool* find(const std::string& name) const = 0;
    virtual std::vector<ITool*> all() const = 0;

    // Builds OpenAI function-calling schema JSON array for the system prompt.
    // Unfiltered variant: returns an entry for every registered tool (used by
    // tests and the system-prompt text builder where filtering is irrelevant).
    virtual nlohmann::json build_schema_json() const = 0;

    // Filtered variant (S3.L): applies `ITool::available(ws)` and
    // `ITool::visible_in_mode(mode)` so the per-turn manifest only exposes
    // tools that actually work in the current workspace + mode.
    virtual nlohmann::json build_schema_json(IWorkspaceServices& ws,
                                             ToolMode mode) const = 0;
};

} // namespace locus
