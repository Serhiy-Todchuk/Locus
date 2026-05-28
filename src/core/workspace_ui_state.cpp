#include "core/workspace_ui_state.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <system_error>

namespace locus {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
fs::path state_path(const fs::path& locus_dir)
{
    return locus_dir / "ui_state.json";
}
} // namespace

WorkspaceUiState load_workspace_ui_state(const fs::path& locus_dir)
{
    WorkspaceUiState s;
    auto path = state_path(locus_dir);
    std::error_code ec;
    if (!fs::exists(path, ec)) return s;

    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("workspace_ui_state: cannot open {}", path.string());
        return s;
    }
    try {
        json j = json::parse(f);
        if (j.contains("open_tabs") && j["open_tabs"].is_array()) {
            for (const auto& t : j["open_tabs"]) {
                if (!t.is_object()) continue;
                WorkspaceUiState::OpenTab ot;
                ot.session_id = t.value("session_id", std::string{});
                ot.title      = t.value("title",      std::string{});
                ot.active     = t.value("active",     false);
                if (!ot.session_id.empty())
                    s.open_tabs.push_back(std::move(ot));
            }
        }
        // S6.18 G.2 -- per-workspace AUI perspective. Missing field is
        // fine; first save_workspace_ui_state will write it.
        if (j.contains("aui_perspective") && j["aui_perspective"].is_string())
            s.aui_perspective = j["aui_perspective"].get<std::string>();
    } catch (const json::exception& e) {
        spdlog::warn("workspace_ui_state: parse error in {}: {} -- using defaults",
                     path.string(), e.what());
        return WorkspaceUiState{};
    }
    return s;
}

bool save_workspace_ui_state(const fs::path& locus_dir,
                             const WorkspaceUiState& state)
{
    std::error_code ec;
    fs::create_directories(locus_dir, ec);

    json arr = json::array();
    for (const auto& t : state.open_tabs) {
        if (t.session_id.empty()) continue;  // never persist empty tabs
        arr.push_back({
            {"session_id", t.session_id},
            {"title",      t.title},
            {"active",     t.active}
        });
    }

    json j = {
        {"open_tabs",       arr},
        {"aui_perspective", state.aui_perspective}
    };

    auto path = state_path(locus_dir);
    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("workspace_ui_state: cannot write {}", path.string());
        return false;
    }
    f << j.dump(2) << '\n';
    return true;
}

} // namespace locus
