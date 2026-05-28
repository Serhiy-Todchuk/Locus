#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace locus {

// S5.I -- per-workspace UI state persisted at `.locus/ui_state.json`. Distinct
// from the global `~/.locus/ui_state.json` which carries window geometry +
// AUI perspective (shared across every workspace). This file tracks
// "what tabs were open the last time this workspace closed" so the next
// open can restore them.
struct WorkspaceUiState {
    struct OpenTab {
        std::string session_id; // empty for never-saved empty tabs (skipped)
        std::string title;      // user-facing label captured at close-time
        bool        active = false;
    };
    std::vector<OpenTab> open_tabs;

    // S6.18 G.2 -- wxAuiManager::SavePerspective() output. Empty = no
    // saved perspective. Moved here from the global ui_state.json so each
    // workspace can carry its own docking layout; the global file was
    // bleeding one workspace's perspective into every other one.
    std::string aui_perspective;
};

WorkspaceUiState load_workspace_ui_state(const std::filesystem::path& locus_dir);
bool             save_workspace_ui_state(const std::filesystem::path& locus_dir,
                                         const WorkspaceUiState& state);

} // namespace locus
