#include "mcp/mcp_config.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>

namespace locus {

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path McpConfigLoader::workspace_config_path(const fs::path& workspace_root)
{
    return workspace_root / ".locus" / "mcp.json";
}

fs::path McpConfigLoader::global_config_path()
{
#ifdef _WIN32
    // %APPDATA% is set on every interactive Windows session; service contexts
    // may not have it, in which case we fall back to %USERPROFILE%.
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return fs::path(appdata) / "Locus" / "mcp.json";
    if (const char* userprofile = std::getenv("USERPROFILE"); userprofile && *userprofile)
        return fs::path(userprofile) / "AppData" / "Roaming" / "Locus" / "mcp.json";
    return {};
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "Locus" / "mcp.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / "Locus" / "mcp.json";
    return {};
#endif
}

std::vector<McpServerConfig> McpConfigLoader::parse_json(const std::string& body)
{
    std::vector<McpServerConfig> out;
    if (body.empty()) return out;

    json j = json::parse(body);
    if (!j.is_object() || !j.contains("mcpServers") || !j["mcpServers"].is_object())
        return out;

    for (auto it = j["mcpServers"].begin(); it != j["mcpServers"].end(); ++it) {
        const auto& entry = it.value();
        if (!entry.is_object()) continue;

        McpServerConfig cfg;
        cfg.name    = it.key();
        cfg.command = entry.value("command", std::string{});
        cfg.cwd     = entry.value("cwd",     std::string{});
        cfg.enabled = entry.value("enabled", true);

        if (entry.contains("args") && entry["args"].is_array()) {
            for (const auto& a : entry["args"]) {
                if (a.is_string()) cfg.args.push_back(a.get<std::string>());
            }
        }
        if (entry.contains("env") && entry["env"].is_object()) {
            for (auto e = entry["env"].begin(); e != entry["env"].end(); ++e) {
                if (e.value().is_string())
                    cfg.env[e.key()] = e.value().get<std::string>();
            }
        }
        if (entry.contains("init_timeout_ms") && entry["init_timeout_ms"].is_number_integer()) {
            int ms = entry["init_timeout_ms"].get<int>();
            if (ms > 0) cfg.init_timeout = std::chrono::milliseconds(ms);
        }

        if (cfg.command.empty()) {
            spdlog::warn("mcp.json: server '{}' has no 'command'; skipping", cfg.name);
            continue;
        }
        out.push_back(std::move(cfg));
    }
    return out;
}

namespace {

std::vector<McpServerConfig> load_one(const fs::path& path)
{
    if (path.empty() || !fs::exists(path)) return {};

    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("mcp.json: cannot open {}", path.string());
        return {};
    }
    std::string body((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    try {
        auto cfgs = McpConfigLoader::parse_json(body);
        spdlog::info("mcp.json: loaded {} server(s) from {}",
                     cfgs.size(), path.string());
        return cfgs;
    } catch (const std::exception& e) {
        spdlog::warn("mcp.json: parse error in {}: {}", path.string(), e.what());
        return {};
    }
}

} // namespace

std::vector<McpServerConfig> McpConfigLoader::load(const fs::path& workspace_root)
{
    auto global_cfgs    = load_one(global_config_path());
    auto workspace_cfgs = load_one(workspace_config_path(workspace_root));

    // Workspace overrides global by name. Walk workspace first so the
    // resulting vector preserves the workspace's own ordering for entries
    // it defines, then append unduplicated global entries at the end.
    std::vector<McpServerConfig> out;
    out.reserve(global_cfgs.size() + workspace_cfgs.size());

    auto already_have = [&](const std::string& name) {
        for (auto& c : out) if (c.name == name) return true;
        return false;
    };

    for (auto& c : workspace_cfgs) out.push_back(std::move(c));
    for (auto& c : global_cfgs) {
        if (!already_have(c.name)) out.push_back(std::move(c));
    }
    return out;
}

} // namespace locus
