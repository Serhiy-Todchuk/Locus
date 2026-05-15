#pragma once

#include "../../../core/workspace.h"

#include <wx/wx.h>

namespace locus {

// Common interface for per-tab settings panels. Each tab panel implements
// all three methods; SettingsDialog calls them in sequence on OK.
class ISettingsPanel {
public:
    virtual ~ISettingsPanel() = default;

    // Reload all UI controls from cfg (used by "Reset to global defaults").
    virtual void load_from_config(const WorkspaceConfig& cfg) = 0;

    // Validate the current control state. Returns true on success; on failure
    // sets out_error and returns false. Currently all panels always return true
    // (server-side validation is the real gate) but the hook exists for future use.
    virtual bool validate(wxString& out_error) const = 0;

    // Write the current control state into cfg. Called by SettingsDialog::on_ok
    // and snapshot_dialog_state in sequence across all panels; panels must not
    // assume cfg is clean on entry (earlier panels may have already written).
    virtual void commit_to_config(WorkspaceConfig& cfg) const = 0;
};

} // namespace locus
