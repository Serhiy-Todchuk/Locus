#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace locus {

class IWorkspaceServices;

// -- Tool approval policy ---------------------------------------------------

enum class ToolApprovalPolicy {
    ask,           // "Ask Every Time" -- show approval UI, require user decision
    auto_approve,  // "Auto-Approve"   -- execute immediately, no user prompt
    deny           // "Deny"           -- never execute, report rejection to the LLM
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
// ITool::visible_in_mode(). Defaults: every tool is visible in `agent` and
// `execute` modes; `plan` and `subagent` are restricted role-specific
// subsets (the plumbing shipped with S3.L; plan-mode lands in S4.D).
enum class ToolMode {
    agent,     // normal interactive turn (chat); full tool catalog
    plan,      // S4.D plan pass -- only propose_plan is visible
    execute,   // S4.D execute pass -- full catalog + mark_step_done
    subagent   // delegated subtask -- role-specific subset
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

    // S5.A -- context-aware description used by the filtered schema builder
    // (the one that already takes `IWorkspaceServices`). Default: return the
    // static description. `SearchTool` overrides to prune the mode list to
    // match the current capability bucket layout (semantic / code-aware off
    // -> drop the corresponding modes from the description text). The
    // schema's `mode` parameter type is unchanged across calls so this does
    // NOT torch the KV cache -- see S4.F.
    virtual std::string description_for(IWorkspaceServices& /*ws*/) const
    {
        return description();
    }

    // Optional hook -- tools that want to ship a richer JSON Schema (nested
    // objects, enums, arrays-of-objects) than the flat `ToolParam` list can
    // express can override this. Default: empty object, in which case the
    // registry derives the schema from `params()`. MCP-proxy tools use this
    // to forward the server-supplied `inputSchema` verbatim.
    virtual nlohmann::json parameters_schema() const { return nlohmann::json::object(); }

    // S5.Z task 7 -- `cancel_flag` is the agent's `cancel_requested_` atomic
    // threaded through so a long-running tool can poll for an in-flight stop.
    // Default `nullptr` keeps test callers and any tool that doesn't care
    // about cancellation back-compat. Long-running shell + MCP tools poll
    // this and abort (Terminate the process tree / CancelIoEx the pipe);
    // pure-CPU fast tools simply ignore it.
    virtual ToolResult execute(const ToolCall& call,
                               IWorkspaceServices& ws,
                               const std::atomic<bool>* cancel_flag = nullptr) = 0;

    // Default approval policy for this tool. User-configured overrides in
    // WorkspaceConfig take precedence -- see AgentCore::resolve_policy().
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
    // `agent` and `execute` (the two "full catalog" modes); `plan` and
    // `subagent` are restricted -- tools that belong there opt in.
    virtual bool visible_in_mode(ToolMode mode) const
    {
        return mode == ToolMode::agent || mode == ToolMode::execute;
    }
};

// -- IToolRegistry interface ------------------------------------------------

class IToolRegistry {
public:
    virtual ~IToolRegistry() = default;

    virtual void   register_tool(std::unique_ptr<ITool> tool) = 0;

    // Remove a previously-registered tool. Returns true if the tool was
    // found and removed, false if no tool by that name exists. Used by
    // McpManager for hot-restart of MCP servers when the server's
    // advertised tool set changes between runs. Callers must guarantee
    // no other thread is mid-call into the tool when unregister fires --
    // for MCP this is enforced by the McpClient stopping (and joining its
    // reader thread) before the manager unregisters its tools.
    virtual bool   unregister_tool(const std::string& name) = 0;

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
