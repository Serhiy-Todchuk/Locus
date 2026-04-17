#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace locus {

class EmbeddingWorker;
class IndexQuery;
class Workspace;

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

    // "always" = pause for user approval; "auto" = execute immediately
    virtual std::string approval_policy() const { return "always"; }

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
