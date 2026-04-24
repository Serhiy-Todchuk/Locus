#pragma once

#include "../tool.h"
#include "../workspace.h"

#include <wx/wx.h>
#include <wx/spinctrl.h>

#include <string>
#include <vector>

namespace locus {

class IToolRegistry;

// Modal dialog for editing workspace + LLM settings.
// Reads from WorkspaceConfig on construction, writes back on OK.
class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow* parent, WorkspaceConfig& config, IToolRegistry& tools);

    // True if user clicked OK and values changed.
    bool config_changed() const { return changed_; }

    // Which parts changed (for the caller to know what to reconfigure).
    bool llm_changed() const { return llm_changed_; }
    bool index_changed() const { return index_changed_; }
    bool semantic_changed() const { return semantic_changed_; }

private:
    void on_ok(wxCommandEvent& evt);

    WorkspaceConfig& config_;
    IToolRegistry&   tools_;
    bool changed_        = false;
    bool llm_changed_    = false;
    bool index_changed_  = false;
    bool semantic_changed_ = false;

    // LLM controls
    wxTextCtrl*        endpoint_ctrl_    = nullptr;
    wxTextCtrl*        model_ctrl_       = nullptr;
    wxSpinCtrlDouble*  temperature_ctrl_ = nullptr;
    wxSpinCtrl*        context_ctrl_     = nullptr;

    // Index controls
    wxTextCtrl*        exclude_ctrl_     = nullptr;

    // Semantic search controls
    wxCheckBox*        semantic_enabled_ctrl_ = nullptr;
    wxTextCtrl*        semantic_model_ctrl_   = nullptr;
    wxCheckBox*        reranker_enabled_ctrl_ = nullptr;
    wxTextCtrl*        reranker_model_ctrl_   = nullptr;
    wxSpinCtrl*        reranker_top_k_ctrl_   = nullptr;

    // Tool approval controls — one choice per tool, same order as tool_names_.
    std::vector<std::string> tool_names_;
    std::vector<wxChoice*>   approval_choices_;
};

} // namespace locus
