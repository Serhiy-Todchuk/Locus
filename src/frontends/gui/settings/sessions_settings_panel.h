#pragma once

#include "settings_panel.h"

#include <wx/wx.h>

namespace locus {

// Settings tab: session-persistence knobs. Currently exposes the opt-in
// activity-log sidecar; expand here when restore_last / auto_cleanup /
// keep_last / delete_after_days need first-class GUIs (they only live in
// `.locus/config.json` today).
class SessionsSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    SessionsSettingsPanel(wxWindow* parent, const WorkspaceConfig& config);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    wxCheckBox* persist_activity_ = nullptr;
    wxCheckBox* restore_last_     = nullptr;
};

} // namespace locus
