#pragma once

#include "settings_panel.h"

#include <wx/spinctrl.h>
#include <wx/wx.h>

namespace locus {

// Settings tab: LLM endpoint, model, temperature, context limit, max tokens,
// tool-call format, sampler overrides, and the model-card preset picker.
class LlmSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    LlmSettingsPanel(wxWindow* parent, const WorkspaceConfig& config);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    void on_preset_choice(wxCommandEvent&);
    void on_preset_apply(wxCommandEvent&);
    void on_sampler_reset(wxCommandEvent&);

    wxTextCtrl*        endpoint_ctrl_    = nullptr;
    wxTextCtrl*        model_ctrl_       = nullptr;
    wxSpinCtrlDouble*  temperature_ctrl_ = nullptr;
    wxSpinCtrl*        context_ctrl_     = nullptr;
    wxSpinCtrl*        max_tokens_ctrl_  = nullptr;
    wxSpinCtrl*        timeout_ctrl_     = nullptr;  // stream-stall watchdog, seconds
    wxChoice*          preset_ctrl_      = nullptr;
    wxStaticText*      preset_hint_      = nullptr;
    wxChoice*          tool_format_ctrl_  = nullptr;
    wxChoice*          grammar_mode_ctrl_ = nullptr;
    wxSpinCtrlDouble*  top_p_ctrl_           = nullptr;
    wxSpinCtrl*        top_k_ctrl_           = nullptr;
    wxSpinCtrlDouble*  min_p_ctrl_           = nullptr;
    wxSpinCtrlDouble*  repeat_penalty_ctrl_  = nullptr;
    wxSpinCtrlDouble*  frequency_penalty_ctrl_ = nullptr;
    wxSpinCtrlDouble*  presence_penalty_ctrl_  = nullptr;
    // S6.10 Task H -- resets the sampler block ONLY (preserves temperature
    // and other non-sampler fields that the user may have hand-tuned).
    wxButton*          sampler_reset_btn_      = nullptr;
    // S6.10 Task F -- auto-detect preset on workspace open.
    wxCheckBox*        auto_detect_preset_ctrl_ = nullptr;
};

} // namespace locus
