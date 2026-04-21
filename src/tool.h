#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace locus {

class EmbeddingWorker;
class IndexQuery;
class Workspace;

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

// Lightweight context passed to every tool execution.
// Tools use this to resolve paths, query the index, etc.
struct WorkspaceContext {
    std::filesystem::path root;
    IndexQuery*           index = nullptr;     // may be null if index unavailable
    EmbeddingWorker*      embedder = nullptr;  // may be null if semantic search disabled
    Workspace*            workspace = nullptr;  // for dynamic state (e.g. hot-toggled features)
};

// -- ITool interface --------------------------------------------------------

class ITool {
public:
    virtual ~ITool() = default;

    virtual std::string              name()        const = 0;
    virtual std::string              description() const = 0;
    virtual std::vector<ToolParam>   params()      const = 0;

    virtual ToolResult execute(const ToolCall& call,
                               const WorkspaceContext& ws) = 0;

    // Default approval policy for this tool. User-configured overrides in
    // WorkspaceConfig take precedence — see AgentCore::resolve_policy().
    virtual ToolApprovalPolicy approval_policy() const { return ToolApprovalPolicy::ask; }

    // Human-readable preview of what this call will do (shown in approval UI)
    virtual std::string preview(const ToolCall& call) const { return ""; }
};

// -- IToolRegistry interface ------------------------------------------------

class IToolRegistry {
public:
    virtual ~IToolRegistry() = default;

    virtual void   register_tool(std::unique_ptr<ITool> tool) = 0;
    virtual ITool* find(const std::string& name) const = 0;
    virtual std::vector<ITool*> all() const = 0;

    // Builds OpenAI function-calling schema JSON array for the system prompt.
    virtual nlohmann::json build_schema_json() const = 0;
};

} // namespace locus
