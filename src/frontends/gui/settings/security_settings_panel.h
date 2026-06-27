#pragma once

#include "settings_panel.h"

#include <wx/wx.h>

namespace locus {

// S6.0 -- Settings tab for the prompt-injection scanner + ingress policy.
// All four knobs map onto WorkspaceConfig::Security. The scanner is a
// transparency tripwire (not a boundary -- the approval gate is); this panel
// just exposes the dials the user previously had to edit in .locus/config.json.
class SecuritySettingsPanel : public wxPanel, public ISettingsPanel {
public:
    SecuritySettingsPanel(wxWindow* parent, const WorkspaceConfig& config);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    wxCheckBox*  injection_scan_   = nullptr;
    wxCheckBox*  scan_zim_         = nullptr;
    wxTextCtrl*  block_confidence_ = nullptr;  // float 0..1
    wxTextCtrl*  max_scan_kb_      = nullptr;   // int
};

} // namespace locus
