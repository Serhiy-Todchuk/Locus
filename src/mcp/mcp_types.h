#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace locus {

// Configuration for a single MCP server (one entry in mcp.json's
// "mcpServers" map). The key in the map becomes `name`.
struct McpServerConfig {
    std::string                        name;
    std::string                        command;
    std::vector<std::string>           args;
    std::map<std::string, std::string> env;
    std::string                        cwd;          // optional override; default = workspace root
    bool                               enabled = true;

    // Per-server initialise timeout. The default (10s) is generous enough
    // for `npx -y @scope/server` first-runs where npm has to download the
    // package; servers that fail to send `initialize` reply in this window
    // are treated as crashed.
    std::chrono::milliseconds          init_timeout = std::chrono::milliseconds(10000);
};

// One tool advertised by an MCP server (parsed from `tools/list` response).
// inputSchema is the raw JSON Schema object MCP returns; we forward it as-is
// to the LLM (after normalisation by ToolRegistry).
struct McpToolDefinition {
    std::string    name;            // server-side name, e.g. "query"
    std::string    description;
    nlohmann::json input_schema;    // JSON Schema "type":"object" shape
};

} // namespace locus
