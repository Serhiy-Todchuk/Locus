#pragma once

#include <string>

namespace locus {

// Per-user persisted UI state. Window geometry lives at
// `~/.locus/ui_state.json` (global -- screen-bound, not workspace-bound).
// The wxAuiManager perspective string moved per-workspace in S6.18 G.2;
// see `WorkspaceUiState::aui_perspective` in
// [src/core/workspace_ui_state.h](../../core/workspace_ui_state.h). It
// lives there because different workspaces genuinely want different
// docking layouts -- the global perspective was bleeding one workspace's
// layout into every other one.
//
// Empty/sentinel values (negative w/h) mean "no saved state, use compiled
// defaults" -- safe for first launch and for users migrating from a
// pre-feature build.
struct UiState {
    // Window geometry. -1 in any field means "no saved value, use default".
    int  window_x         = -1;
    int  window_y         = -1;
    int  window_width     = -1;
    int  window_height    = -1;
    bool window_maximized = false;

    // Held here for source-compat with code that still reads the in-memory
    // struct directly. NEVER round-trips through load_ui_state /
    // save_ui_state after S6.18 G.2 -- the per-workspace path is the
    // source of truth. LocusFrame populates this field manually from the
    // workspace store after construction.
    std::string aui_perspective;
};

// Load window geometry from the global `~/.locus/ui_state.json`.
// `aui_perspective` is left empty -- the per-workspace store
// (WorkspaceUiState) carries it now. Returns a default-constructed
// UiState when the file is missing or unparseable. Never throws.
UiState load_ui_state();

// Persist window geometry to the global `~/.locus/ui_state.json`.
// Only the window_* fields are written; the legacy `aui_perspective` key
// in that file is dropped here (G.2 migration). Returns true on success.
bool save_ui_state(const UiState& state);

} // namespace locus
