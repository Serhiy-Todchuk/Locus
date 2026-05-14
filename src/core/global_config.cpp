#include "core/global_config.h"
#include "core/global_paths.h"
#include "core/workspace_config_json.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

namespace locus {

namespace fs = std::filesystem;
using json = nlohmann::json;

GlobalConfig load_global_config_or_defaults()
{
    auto path = global_paths::config_path();
    if (path.empty() || !fs::exists(path)) {
        return GlobalConfig{};
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("Global config: cannot open {}", path.string());
        return GlobalConfig{};
    }

    try {
        json j = json::parse(f);
        auto cfg = workspace_config_from_json(j);
        spdlog::info("Loaded global config from {}", path.string());
        return cfg;
    } catch (const json::exception& e) {
        spdlog::warn("Global config: parse error in {}: {} -- using defaults",
                     path.string(), e.what());
        return GlobalConfig{};
    }
}

bool save_global_config(const GlobalConfig& cfg)
{
    auto path = global_paths::config_path();
    if (path.empty()) {
        spdlog::warn("Global config: home directory unresolved, cannot save");
        return false;
    }

    // Strip mcp:* approval entries before serialising -- they reference
    // workspace-specific server names. See S5.M plan for rationale.
    GlobalConfig filtered = cfg;
    for (auto it = filtered.tool_approval_policies.begin();
         it != filtered.tool_approval_policies.end();) {
        if (it->first.rfind("mcp:", 0) == 0) {
            it = filtered.tool_approval_policies.erase(it);
        } else {
            ++it;
        }
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // ec ignored: open below will fail loudly if the dir really isn't usable.

    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("Global config: cannot write to {}", path.string());
        return false;
    }
    f << workspace_config_to_json(filtered).dump(2) << '\n';
    spdlog::info("Saved global config to {}", path.string());
    return true;
}

} // namespace locus
