#pragma once

#include <string>

namespace locus {

// Per-user persisted UI state. Lives at `~/.locus/ui_state.json`. Separate
// from the global config because it changes every time the user moves a
// window or docks a pane -- noisy to diff, noisy to "save as defaults", and
// nothing the user types into the file by hand.
//
// Persisted across runs:
//   - main window geometry (position + size + maximized flag)
//   - wxAuiManager perspective string (pane positions, sizes, hidden/shown)
//
// Empty/sentinel values (negative w/h, empty perspective) mean "no saved
// state, use compiled defaults" -- safe for first launch and for users
// migrating from a pre-feature build.
struct UiState {
    // Window geometry. -1 in any field means "no saved value, use default".
    int  window_x         = -1;
    int  window_y         = -1;
    int  window_width     = -1;
    int  window_height    = -1;
    bool window_maximized = false;

    // wxAuiManager::SavePerspective() output. Empty = no saved perspective.
    std::string aui_perspective;
};

// Load from ~/.locus/ui_state.json. Returns a default-constructed UiState
// when the file is missing or unparseable. Never throws.
UiState load_ui_state();

// Persist to ~/.locus/ui_state.json. Returns true on success.
bool save_ui_state(const UiState& state);

} // namespace locus
