#pragma once

#include "settings_panel.h"

#include <wx/wx.h>

namespace locus {

// Settings tab: five capability bucket checkboxes (S5.A).
class CapabilitiesSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    CapabilitiesSettingsPanel(wxWindow* parent, const WorkspaceConfig& config);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    wxCheckBox* cap_bg_       = nullptr;
    wxCheckBox* cap_semantic_ = nullptr;
    wxCheckBox* cap_code_     = nullptr;
    wxCheckBox* cap_memory_   = nullptr;
    wxCheckBox* cap_web_      = nullptr;
};

} // namespace locus
