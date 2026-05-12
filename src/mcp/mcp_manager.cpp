#include "mcp/mcp_manager.h"

#include "mcp/mcp_config.h"
#include "mcp/mcp_tool.h"
#include "../tools/tool_registry.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace locus {

namespace fs = std::filesystem;

McpManager::McpManager(IToolRegistry& registry, fs::path workspace_root)
    : registry_(registry), root_(std::move(workspace_root))
{
}

McpManager::~McpManager()
{
    stop_all();
}

std::size_t McpManager::server_count() const
{
    std::lock_guard l(mu_);
    return servers_.size();
}

void McpManager::start_all()
{
    std::vector<McpServerConfig> cfgs = McpConfigLoader::load(root_);

    {
        std::lock_guard l(mu_);
        if (started_) {
            spdlog::warn("McpManager: start_all() already called; ignoring");
            return;
        }
        started_ = true;

        for (auto& cfg : cfgs) {
            auto entry = std::make_unique<ServerEntry>();
            entry->cfg = std::move(cfg);
            entry->client = std::make_unique<McpClient>(entry->cfg);
            servers_.push_back(std::move(entry));
        }

        for (auto& e : servers_) {
            if (!e->cfg.enabled) {
                spdlog::info("McpManager[{}]: disabled in config", e->cfg.name);
                continue;
            }
            start_one_locked(*e);
        }
    }

    // Log a one-line summary at info level outside the lock.
    auto snap = status_snapshot();
    int ready = 0, failed = 0, disabled = 0, total_tools = 0;
    for (auto& s : snap) {
        if (s.status == McpClient::Status::ready)  { ++ready;  total_tools += static_cast<int>(s.tool_names.size()); }
        else if (s.status == McpClient::Status::failed) ++failed;
        if (!s.last_error.empty() && s.status == McpClient::Status::not_started) ++disabled;
    }
    if (!snap.empty()) {
        spdlog::info("McpManager: {} server(s), {} ready ({} tools), {} failed",
                     snap.size(), ready, total_tools, failed);
    }
}

void McpManager::start_one_locked(ServerEntry& e)
{
    spdlog::info("McpManager[{}]: starting '{}'", e.cfg.name, e.cfg.command);

    // Hook the crash callback BEFORE start() so a fast-failing server still
    // emits a status event the GUI can pick up.
    auto* client_ptr = e.client.get();
    auto* entry_ptr  = &e;
    e.client->on_crash([this, entry_ptr]() {
        std::lock_guard l(mu_);
        emit_status_locked(*entry_ptr);
    });

    if (!e.client->start(root_)) {
        spdlog::warn("McpManager[{}]: start failed: {}", e.cfg.name, client_ptr->last_error());
        emit_status_locked(e);
        return;
    }

    std::vector<McpToolDefinition> defs;
    try {
        defs = client_ptr->list_tools();
    } catch (const std::exception& ex) {
        spdlog::warn("McpManager[{}]: tools/list failed: {}", e.cfg.name, ex.what());
        client_ptr->stop();
        emit_status_locked(e);
        return;
    }

    register_tools_locked(e, defs);
    emit_status_locked(e);
}

void McpManager::register_tools_locked(ServerEntry& e,
                                       const std::vector<McpToolDefinition>& defs)
{
    auto* client_ptr = e.client.get();

    // On hot-restart the stale McpTool entries borrow pointers into the
    // dead McpClient. Unregister them up front -- the new client gets a
    // fresh registration for whatever its `tools/list` advertises (which
    // may be a different set than the previous run advertised).
    unregister_tools_locked(e);

    for (const auto& def : defs) {
        std::string namespaced = "mcp:" + e.cfg.name + ":" + def.name;

        try {
            registry_.register_tool(std::make_unique<McpTool>(client_ptr, def));
            e.namespaced_tool_names.push_back(namespaced);
            spdlog::trace("McpManager[{}]: registered tool '{}'", e.cfg.name, namespaced);
        } catch (const std::exception& ex) {
            spdlog::warn("McpManager[{}]: failed to register tool '{}': {}",
                         e.cfg.name, namespaced, ex.what());
        }
    }
    spdlog::info("McpManager[{}]: registered {} tool(s)",
                 e.cfg.name, e.namespaced_tool_names.size());
}

void McpManager::unregister_tools_locked(ServerEntry& e)
{
    for (auto& namespaced : e.namespaced_tool_names) {
        if (registry_.unregister_tool(namespaced))
            spdlog::trace("McpManager[{}]: unregistered tool '{}'",
                          e.cfg.name, namespaced);
    }
    e.namespaced_tool_names.clear();
}

void McpManager::stop_all()
{
    std::vector<std::unique_ptr<ServerEntry>> drained;
    {
        std::lock_guard l(mu_);
        drained.swap(servers_);
        started_ = false;
    }
    // Stop the McpClient first so its reader thread is joined and no further
    // crash callback can fire on the listener. Then unregister the tools so
    // the agent's tool registry can't hand out McpTool entries that borrow
    // a torn-down client pointer.
    for (auto& e : drained) {
        if (e->client) e->client->stop();
        for (auto& namespaced : e->namespaced_tool_names) {
            registry_.unregister_tool(namespaced);
        }
        e->namespaced_tool_names.clear();
    }
    if (!drained.empty())
        spdlog::info("McpManager: stopped {} server(s)", drained.size());
}

bool McpManager::restart(const std::string& server_name)
{
    std::lock_guard l(mu_);
    for (auto& e : servers_) {
        if (e->cfg.name != server_name) continue;
        if (e->client) e->client->stop();
        e->client = std::make_unique<McpClient>(e->cfg);
        start_one_locked(*e);
        return e->client->status() == McpClient::Status::ready;
    }
    return false;
}

std::vector<McpManager::ServerStatus> McpManager::status_snapshot() const
{
    std::lock_guard l(mu_);
    std::vector<ServerStatus> out;
    out.reserve(servers_.size());
    for (auto& e : servers_) {
        ServerStatus s;
        s.name          = e->cfg.name;
        s.command       = e->cfg.command;
        s.status        = e->client ? e->client->status() : McpClient::Status::not_started;
        s.last_error    = e->client ? e->client->last_error() : std::string{};
        s.tool_names    = e->namespaced_tool_names;
        s.has_exit_code = e->client ? e->client->has_exit_code() : false;
        s.exit_code     = e->client ? e->client->exit_code() : 0;
        out.push_back(std::move(s));
    }
    return out;
}

void McpManager::emit_status_locked(const ServerEntry& e)
{
    if (!listener_) return;

    ServerStatus s;
    s.name          = e.cfg.name;
    s.command       = e.cfg.command;
    s.status        = e.client ? e.client->status() : McpClient::Status::not_started;
    s.last_error    = e.client ? e.client->last_error() : std::string{};
    s.tool_names    = e.namespaced_tool_names;
    s.has_exit_code = e.client ? e.client->has_exit_code() : false;
    s.exit_code     = e.client ? e.client->exit_code() : 0;

    // Listener fires on the locked thread. If the GUI listener wants to
    // touch wx widgets it must marshal back to the UI thread itself.
    try {
        listener_(s);
    } catch (const std::exception& ex) {
        spdlog::warn("McpManager: status listener threw: {}", ex.what());
    }
}

} // namespace locus
