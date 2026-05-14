#pragma once

#include "../../core/frontend.h"

#include <wx/wx.h>
#include <wx/stc/stc.h>

#include <functional>
#include <string>

namespace locus {

// Callback when the user makes a decision.
// decision: approve, reject, or modify
// modified_args: non-empty JSON when decision == modify
using ToolDecisionCallback = std::function<void(
    const std::string& call_id,
    ToolDecision decision,
    const nlohmann::json& modified_args)>;

// Modal dialog shown when a tool call needs approval. Replaced the bottom-
// docked AUI pane in S5.Z #6 -- the pane was always present (just hidden when
// idle), wasting screen real estate, and offered no way for the user to set
// the surface aside without choosing approve/modify/reject. The dialog form:
//   * appears centred over the frame only when a decision is needed
//   * blocks until the user decides (the agent thread is condvar-blocked
//     in ToolDispatcher anyway, so "scroll chat while deciding" was largely
//     cosmetic)
//   * leaves zero permanent footprint when no call is pending
//
// Keyboard: Enter=Approve, Esc=Reject, M=Modify (toggle edit mode).
//
// For ask_user: shows the question with a text input instead of JSON args.
//
// Use the static run() helper from the frame -- it constructs, populates,
// and ShowModal()s in one call, returning only after the user has decided.
class ToolApprovalDialog : public wxDialog {
public:
    // Construct + populate + ShowModal in one shot. Returns once the user has
    // made a decision (or closed the window, treated as reject so the agent
    // can unblock). on_decision is invoked exactly once before this returns.
    // `safety_warnings` (S4.V Task 5) contains flagged outside-workspace path
    // tokens parsed out of run_command-family args; when non-empty, the dialog
    // renders a yellow banner above the preview.
    static void run(wxWindow* parent,
                    const std::string& call_id,
                    const std::string& tool_name,
                    const nlohmann::json& args,
                    const std::string& preview,
                    const std::vector<std::string>& safety_warnings,
                    ToolDecisionCallback on_decision);

private:
    ToolApprovalDialog(wxWindow* parent, ToolDecisionCallback on_decision);

    void create_controls();
    void layout_normal();    // standard tool approval layout
    void layout_ask_user();  // ask_user: question + text input

    // Populate + show contents for a pending tool call.
    void show_tool_call(const std::string& call_id,
                        const std::string& tool_name,
                        const nlohmann::json& args,
                        const std::string& preview,
                        const std::vector<std::string>& safety_warnings);

    // S4.A: render args_stc_ as a red/green unified-diff view when the pending
    // call is edit_file. Returns true if the diff view was used (caller falls
    // back to JSON view when false).
    bool render_diff_view();
    void render_json_view();

    void on_approve(wxCommandEvent& evt);
    void on_reject(wxCommandEvent& evt);
    void on_modify(wxCommandEvent& evt);
    void on_key(wxKeyEvent& evt);
    void on_close(wxCloseEvent& evt);

    // Single funnel for "decision made, close the dialog". All button handlers
    // and the close-window handler route through here so the agent gets exactly
    // one tool_decision call regardless of how the dialog was dismissed.
    void send_decision(ToolDecision decision,
                       const nlohmann::json& modified_args = {});

    ToolDecisionCallback on_decision_;

    // Current tool call state.
    std::string    call_id_;
    std::string    tool_name_;
    nlohmann::json original_args_;
    bool           editing_       = false;
    // Guard against double-fire (e.g. the user clicks Approve and then the
    // close-window handler runs as the dialog tears down). The agent's
    // condvar would re-pulse a second tool_decision into a now-stale call.
    bool           decision_sent_ = false;

    // Controls -- created once, shown/hidden per mode.
    wxStaticText*     name_label_    = nullptr;
    wxStaticText*     warning_label_ = nullptr;  // S4.V outside-workspace banner
    wxStaticText*     preview_label_ = nullptr;
    wxStyledTextCtrl* args_stc_      = nullptr;
    wxTextCtrl*       ask_input_     = nullptr;  // for ask_user mode
    wxButton*         btn_approve_   = nullptr;
    wxButton*         btn_reject_    = nullptr;
    wxButton*         btn_modify_    = nullptr;
    wxBoxSizer*       main_sizer_    = nullptr;
};

} // namespace locus
