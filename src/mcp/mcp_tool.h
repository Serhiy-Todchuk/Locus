#pragma once

#include "mcp/mcp_client.h"
#include "mcp/mcp_types.h"
#include "../tools/tool.h"

namespace locus {

// ITool wrapper around a single MCP-advertised tool. The `name()` reported
// to the LLM is the namespaced form `mcp:<server>:<tool>` -- this keeps
// MCP tool names from colliding with built-ins, makes the source of any
// given call obvious in logs, and lets the existing per-tool approval
// override map address them naturally.
//
// Lifetime: owned by McpManager. The McpClient pointer is borrowed; the
// manager guarantees it outlives the tool by stopping the client and
// unregistering its tools together.
class McpTool : public ITool {
public:
    McpTool(McpClient* client, McpToolDefinition def);

    std::string              name()        const override { return namespaced_; }
    std::string              description() const override;
    std::vector<ToolParam>   params()      const override;

    ToolResult execute(const ToolCall& call, IWorkspaceServices& ws) override;

    // MCP tools are untrusted by default. Users opt in per-tool via the
    // existing per-workspace `tool_approvals` config map (key =
    // `mcp:<server>:<tool>`).
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::ask; }

    std::string preview(const ToolCall& call) const override;

    // The MCP catalogue is server-driven; if the server crashed we hide
    // its tools from the per-turn manifest so the LLM doesn't try to call
    // something we know is gone.
    bool available(IWorkspaceServices& ws) const override;

    // Accessor for the manager / settings UI.
    const McpToolDefinition& definition() const { return def_; }
    const McpClient*         client()     const { return client_; }
    const std::string&       server_name()const { return server_name_; }

private:
    McpClient*         client_;
    McpToolDefinition  def_;
    std::string        server_name_;
    std::string        namespaced_;     // "mcp:<server>:<tool>"
};

} // namespace locus
