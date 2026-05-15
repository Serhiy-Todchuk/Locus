#pragma once

#include "settings_panel.h"

#include <wx/spinctrl.h>
#include <wx/wx.h>

namespace locus {

// Settings tab: exclude patterns, semantic search toggle + model, reranker.
class IndexSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    IndexSettingsPanel(wxWindow* parent, const WorkspaceConfig& config);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    wxTextCtrl*   exclude_ctrl_          = nullptr;
    wxCheckBox*   semantic_enabled_ctrl_ = nullptr;
    wxTextCtrl*   semantic_model_ctrl_   = nullptr;
    wxCheckBox*   reranker_enabled_ctrl_ = nullptr;
    wxTextCtrl*   reranker_model_ctrl_   = nullptr;
    wxSpinCtrl*   reranker_top_k_ctrl_   = nullptr;
};

} // namespace locus
