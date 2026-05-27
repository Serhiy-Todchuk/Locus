#pragma once

// S6.11 -- describe_tool meta-tool.
//
// When `WorkspaceConfig::Agent::lazy_tool_manifest` is on, the per-turn tool catalog
// (system prompt + API tools array) is trimmed to one-line summaries. The
// model fetches the full schema of a tool it wants to call via
// `describe_tool('<name>')`. Result content is the same OpenAI-format JSON
// schema entry that the full-manifest path would have shipped up-front.
//
// Visibility: always REGISTERED in the tool registry. `available(ws)` returns
// `ws.config().agent.lazy_tool_manifest`, so the LLM-facing manifest only includes
// the tool when lazy mode is on. Inspection via the tools panel / slash
// commands works in any mode (those bypass the manifest filter).
//
// See architecture/decisions/0007-context-budget-reshape-lazy-manifest-and-profiles.md.

#include "tool.h"

namespace locus {

class DescribeTool : public ITool {
public:
    // Holds a non-owning reference to the registry it inspects. Construction
    // order: registry exists first, DescribeTool inserted into it last. The
    // registry outlives every tool it owns.
    explicit DescribeTool(const IToolRegistry& registry) : registry_(registry) {}

    std::string                  name() const override;
    std::string                  description() const override;
    std::string                  short_description() const override {
        return "describe_tool(name) -- return full JSON schema for another tool.";
    }
    std::vector<ToolParam>       params() const override;
    ToolApprovalPolicy           approval_policy() const override;
    ToolResult                   execute(const ToolCall& call,
                                         IWorkspaceServices& ws,
                                         const std::atomic<bool>* cancel_flag) override;

    // available(ws) returns ws.config().agent.lazy_tool_manifest. The meta-tool
    // serves no purpose when full schemas are already inline, so we hide it
    // from the LLM in that case.
    bool available(IWorkspaceServices& ws) const override;

private:
    const IToolRegistry& registry_;
};

} // namespace locus
