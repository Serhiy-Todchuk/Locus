#pragma once

#include "settings_panel.h"

#include <wx/wx.h>

namespace locus {

// Settings tab: sound-alert toggles for moments that need the user's
// attention (tool approval, ask_user, turn complete, compaction-needed).
// Each event maps to a distinct Windows system-sound alias; settings here
// only control which events fire and whether to gate on focus.
class NotificationsSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    NotificationsSettingsPanel(wxWindow* parent, const WorkspaceConfig& config);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    wxCheckBox* snd_tool_approval_ = nullptr;
    wxCheckBox* snd_ask_user_      = nullptr;
    wxCheckBox* snd_turn_complete_ = nullptr;
    wxCheckBox* snd_compaction_    = nullptr;
    wxCheckBox* only_unfocused_    = nullptr;
};

} // namespace locus
