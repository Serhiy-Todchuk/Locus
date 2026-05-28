#include "ui_state.h"
#include "../../core/global_paths.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

namespace locus {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
fs::path global_ui_state_path()
{
    auto d = global_paths::global_dir();
    return d.empty() ? fs::path{} : d / "ui_state.json";
}
} // namespace

UiState load_ui_state()
{
    auto path = global_ui_state_path();
    if (path.empty() || !fs::exists(path)) return UiState{};

    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("ui_state: cannot open {}", path.string());
        return UiState{};
    }

    try {
        json j = json::parse(f);
        UiState s;
        if (j.contains("window") && j["window"].is_object()) {
            const auto& w = j["window"];
            if (w.contains("x") && w["x"].is_number_integer())
                s.window_x = w["x"].get<int>();
            if (w.contains("y") && w["y"].is_number_integer())
                s.window_y = w["y"].get<int>();
            if (w.contains("width") && w["width"].is_number_integer())
                s.window_width = w["width"].get<int>();
            if (w.contains("height") && w["height"].is_number_integer())
                s.window_height = w["height"].get<int>();
            if (w.contains("maximized") && w["maximized"].is_boolean())
                s.window_maximized = w["maximized"].get<bool>();
        }
        // aui_perspective is intentionally NOT loaded here -- it moved to the
        // per-workspace file in S6.18 G.2. load_workspace_ui_state() reads it
        // and handles migration from the legacy global value.
        return s;
    } catch (const json::exception& e) {
        spdlog::warn("ui_state: parse error in {}: {} -- using defaults",
                     path.string(), e.what());
        return UiState{};
    }
}

bool save_ui_state(const UiState& state)
{
    auto path = global_ui_state_path();
    if (path.empty()) {
        spdlog::warn("ui_state: home dir unresolved, cannot save");
        return false;
    }

    // Window-only -- aui_perspective lives per-workspace now (G.2).
    json j = {
        {"window", {
            {"x",         state.window_x},
            {"y",         state.window_y},
            {"width",     state.window_width},
            {"height",    state.window_height},
            {"maximized", state.window_maximized}
        }}
    };

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("ui_state: cannot write to {}", path.string());
        return false;
    }
    f << j.dump(2) << '\n';
    return true;
}

} // namespace locus
