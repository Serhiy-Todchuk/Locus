#pragma once

#include "mcp_types.h"

#include <filesystem>
#include <vector>

namespace locus {

// Loads MCP server configurations from up to two locations and merges them.
// The workspace file (`<root>/.locus/mcp.json`) overrides the global file
// (`%APPDATA%/Locus/mcp.json` on Windows; `$XDG_CONFIG_HOME/Locus/mcp.json`
// or `~/.config/Locus/mcp.json` elsewhere). Override granularity is per
// server name -- if both files declare a server "fs", the workspace wins.
//
// Format mirrors Claude Desktop / Cursor / Cline:
//   { "mcpServers": { "<name>": { "command":"...", "args":[], "env":{}, "enabled":true } } }
//
// Missing files are not errors -- they yield an empty list. JSON parse
// failures log a warning and skip the offending file.
class McpConfigLoader {
public:
    // Returns the merged server list. Disabled servers are still returned
    // so the UI can show them; callers should filter on `enabled`.
    static std::vector<McpServerConfig> load(const std::filesystem::path& workspace_root);

    // Path resolvers exposed for tests and the settings UI.
    static std::filesystem::path workspace_config_path(const std::filesystem::path& workspace_root);
    static std::filesystem::path global_config_path();

    // Parse a single mcp.json string into McpServerConfig entries. Throws
    // nlohmann::json::exception on malformed JSON.
    static std::vector<McpServerConfig> parse_json(const std::string& body);
};

} // namespace locus
