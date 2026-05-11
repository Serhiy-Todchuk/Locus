#pragma once

#include "mcp/mcp_client.h"
#include "mcp/mcp_types.h"
#include "../tools/tool.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace locus {

class IToolRegistry;

// Owns the per-workspace pool of MCP server connections. Constructed by
// LocusSession AFTER the built-in ToolRegistry has been populated, so
// McpManager can register namespaced tools alongside the built-ins.
//
// Lifecycle: ctor reads mcp.json, spawns each enabled server in parallel,
// blocks on initialize handshakes, lists tools, registers them. start()
// returns once every server is either ready or failed. Failed servers
// stay in the manager so the settings UI can show their status -- they
// just don't contribute any tools.
//
// Crash handling: each McpClient gets an on_crash callback that logs a
// warning and emits a manager event. Crashed servers' tools become
// unavailable via McpTool::available()=false; we don't unregister them
// from the registry so the GUI can still display them as "(crashed)".
class McpManager {
public:
    struct ServerStatus {
        std::string         name;
        std::string         command;        // for display only
        McpClient::Status   status;
        std::string         last_error;     // empty when status == ready
        std::vector<std::string> tool_names; // namespaced
    };

    using StatusListener = std::function<void(const ServerStatus&)>;

    McpManager(IToolRegistry& registry,
               std::filesystem::path workspace_root);
    ~McpManager();

    McpManager(const McpManager&)            = delete;
    McpManager& operator=(const McpManager&) = delete;

    // Read the merged mcp.json config and bring all enabled servers up.
    // Already-running servers are not restarted -- start_all() is a one-shot;
    // call stop_all() before reusing the manager.
    void start_all();

    // Terminate every server. Idempotent. ToolRegistry entries are kept --
    // the manager doesn't own the registry and can't unregister.
    void stop_all();

    // Hot-restart a single server by name. Re-registers any changed tool
    // set. Returns false if name is unknown or the spawn failed.
    bool restart(const std::string& server_name);

    // Snapshot of every configured server's current state. Thread-safe.
    std::vector<ServerStatus> status_snapshot() const;

    // Register a callback fired (on the McpClient reader thread, so cheap
    // work only) whenever a server's status changes. Used by the GUI to
    // refresh the settings panel without polling.
    void set_status_listener(StatusListener cb) { listener_ = std::move(cb); }

    std::size_t server_count() const;

private:
    struct ServerEntry {
        McpServerConfig             cfg;
        std::unique_ptr<McpClient>  client;
        std::vector<std::string>    namespaced_tool_names;  // owned by registry, not us
    };

    void start_one_locked(ServerEntry& e);   // pending_mu_ held
    void register_tools_locked(ServerEntry& e,
                               const std::vector<McpToolDefinition>& defs);
    void emit_status_locked(const ServerEntry& e);

    IToolRegistry&         registry_;
    std::filesystem::path  root_;

    mutable std::mutex     mu_;
    std::vector<std::unique_ptr<ServerEntry>> servers_;
    StatusListener         listener_;
    bool                   started_ = false;
};

} // namespace locus
