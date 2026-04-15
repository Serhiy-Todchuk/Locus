#pragma once

#include "../frontend.h"

#include <wx/wx.h>
#include <wx/stc/stc.h>

#include <functional>
#include <string>

namespace locus {

// Callback when the user makes a decision.
// decision: approve, reject, or modify
// modified_args: non-empty JSON string when decision == modify
using ToolDecisionCallback = std::function<void(
    const std::string& call_id,
    ToolDecision decision,
    const nlohmann::json& modified_args)>;

// Panel that slides in between the chat and input areas when a tool call
// needs approval. Shows tool name, preview, JSON args, and action buttons.
//
// For ask_user: shows the question with a text input instead of JSON args.
//
// Keyboard: Enter=Approve, Esc=Reject, M=Modify (toggle edit mode).
class ToolApprovalPanel : public wxPanel {
public:
    ToolApprovalPanel(wxWindow* parent, ToolDecisionCallback on_decision);

    // Show the panel for a new tool call pending approval.
    void show_tool_call(const std::string& call_id,
                        const std::string& tool_name,
                        const nlohmann::json& args,
                        const std::string& preview);

    // Hide the panel after decision is made.
    void dismiss();

private:
    void create_controls();
    void layout_normal();    // standard tool approval layout
    void layout_ask_user();  // ask_user: question + text input

    void on_approve(wxCommandEvent& evt);
    void on_reject(wxCommandEvent& evt);
    void on_modify(wxCommandEvent& evt);
    void on_key(wxKeyEvent& evt);

    // Send decision and dismiss.
    void send_decision(ToolDecision decision);

    ToolDecisionCallback on_decision_;

    // Current tool call state.
    std::string    call_id_;
    std::string    tool_name_;
    nlohmann::json original_args_;
    bool           editing_ = false;

    // Controls — created once, shown/hidden per mode.
    wxStaticText*     name_label_    = nullptr;
    wxStaticText*     preview_label_ = nullptr;
    wxStyledTextCtrl* args_stc_      = nullptr;
    wxTextCtrl*       ask_input_     = nullptr;  // for ask_user mode
    wxButton*         btn_approve_   = nullptr;
    wxButton*         btn_reject_    = nullptr;
    wxButton*         btn_modify_    = nullptr;
    wxBoxSizer*       main_sizer_    = nullptr;
};

} // namespace locus
