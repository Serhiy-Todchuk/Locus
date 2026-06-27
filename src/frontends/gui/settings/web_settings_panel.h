#pragma once

#include "settings_panel.h"

#include <wx/wx.h>

namespace locus {

// S6.1 -- Settings tab for web retrieval (RAG). Maps onto WorkspaceConfig::Web.
//
// Note: the master gate that drops the three web tools from the manifest is the
// `web_retrieval` capability (Capabilities tab), NOT `enabled` here. `enabled`
// is the runtime network kill-switch the tools check; the panel surfaces both
// so the relationship is visible (a hint points the user at Capabilities).
class WebSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    WebSettingsPanel(wxWindow* parent, const WorkspaceConfig& config);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    wxCheckBox* enabled_         = nullptr;
    wxChoice*   provider_        = nullptr;  // brave | searxng
    wxTextCtrl* api_key_         = nullptr;  // password style
    wxTextCtrl* api_url_         = nullptr;
    wxTextCtrl* max_results_     = nullptr;
    wxTextCtrl* cache_ttl_hours_ = nullptr;
    wxTextCtrl* cache_max_mb_    = nullptr;
    wxTextCtrl* max_page_kb_     = nullptr;
    wxCheckBox* allow_http_      = nullptr;
};

} // namespace locus
