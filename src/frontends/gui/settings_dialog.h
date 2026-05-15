#pragma once

#include "../../tools/tool.h"
#include "../../core/workspace.h"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>

#include <functional>
#include <string>
#include <vector>

namespace locus {

class IToolRegistry;
class McpManager;

// Modal dialog for editing workspace + LLM settings.
// Reads from WorkspaceConfig on construction, writes back on OK.
//
// S4.G phase 2: wxNotebook tabs (LLM, Index, Tool Approvals, MCP Servers).
// The MCP tab is populated only when a non-null McpManager is supplied;
// otherwise it shows a hint pointing the user at the mcp.json schema doc.
class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow* parent, WorkspaceConfig& config, IToolRegistry& tools,
                   McpManager* mcp = nullptr);

    // True if user clicked OK and values changed.
    bool config_changed() const { return changed_; }

    // Which parts changed (for the caller to know what to reconfigure).
    bool llm_changed() const { return llm_changed_; }
    bool index_changed() const { return index_changed_; }
    bool semantic_changed() const { return semantic_changed_; }

private:
    // Each builder takes the notebook as the parent for the top-level panel
    // it returns. wxNotebook asserts that every AddPage'd window has the
    // notebook itself as its wxWindow parent -- passing `this` (the dialog)
    // trips a Debug assertion at runtime.
    wxPanel* build_llm_tab(wxWindow* parent);
    wxPanel* build_index_tab(wxWindow* parent);
    wxPanel* build_approvals_tab(wxWindow* parent);
    wxPanel* build_mcp_tab(wxWindow* parent);
    wxPanel* build_capabilities_tab(wxWindow* parent);

    void on_ok(wxCommandEvent& evt);
    void on_save_as_global_defaults(wxCommandEvent& evt);
    void refresh_mcp_list();
    void on_mcp_restart(wxCommandEvent& evt);
    void on_mcp_open_json(wxCommandEvent& evt);
    void on_mcp_select(wxListEvent& evt);

    // S5.M -- snapshots the current dialog state into a WorkspaceConfig.
    // Starts from `config_` so untouched fields survive; overwrites every
    // field the dialog manages. Deliberately excludes MCP trust toggles
    // (`mcp:*` approval keys) -- those are workspace-specific and the
    // global-save path filters them out anyway.
    WorkspaceConfig snapshot_dialog_state() const;

    // Per-tab "Reset to global defaults" handlers. Each reloads its tab's
    // controls from the supplied global config and leaves the other tabs
    // alone. OK still has to be pressed to commit; Cancel discards.
    // The MCP variant clears all trust toggles because global config
    // never carries mcp:* approval entries (filtered on save).
    void reset_llm_tab_to(const WorkspaceConfig& cfg);
    void reset_index_tab_to(const WorkspaceConfig& cfg);
    void reset_capabilities_tab_to(const WorkspaceConfig& cfg);
    void reset_approvals_tab_to(const WorkspaceConfig& cfg);
    void reset_mcp_tab_to(const WorkspaceConfig& cfg);

    // Appends a right-aligned "Reset to global defaults" button to `outer`.
    // The lambda is called with the freshly-loaded global config each click.
    void add_reset_button(wxPanel* panel, wxBoxSizer* outer,
                          std::function<void(const WorkspaceConfig&)> on_reset);

    WorkspaceConfig& config_;
    IToolRegistry&   tools_;
    McpManager*      mcp_;

    bool changed_        = false;
    bool llm_changed_    = false;
    bool index_changed_  = false;
    bool semantic_changed_     = false;
    bool capabilities_changed_ = false;

public:
    bool capabilities_changed() const { return capabilities_changed_; }
private:

    // LLM controls
    wxTextCtrl*        endpoint_ctrl_    = nullptr;
    wxTextCtrl*        model_ctrl_       = nullptr;
    wxSpinCtrlDouble*  temperature_ctrl_ = nullptr;
    wxSpinCtrl*        context_ctrl_     = nullptr;
    wxSpinCtrl*        max_tokens_ctrl_  = nullptr;
    // S4.V Task 6 -- model-card presets row.
    wxChoice*          preset_ctrl_      = nullptr;
    wxStaticText*      preset_hint_      = nullptr;
    // S4.V Task 6 -- expose ToolFormat so the user (and the preset apply)
    // can see what the wire format hint is. Default: Auto.
    wxChoice*          tool_format_ctrl_ = nullptr;
    // S4.V Task 7 -- power-user sampler overrides. 0 = "don't send"
    // (server's per-model defaults apply); positive values are forwarded
    // to the LLM request body. Field names match llama.cpp / LM Studio.
    wxSpinCtrlDouble*  top_p_ctrl_           = nullptr;
    wxSpinCtrl*        top_k_ctrl_           = nullptr;
    wxSpinCtrlDouble*  min_p_ctrl_           = nullptr;
    wxSpinCtrlDouble*  repeat_penalty_ctrl_  = nullptr;
    // OpenAI-protocol penalties (range -2..2). 0 = "don't send".
    wxSpinCtrlDouble*  frequency_penalty_ctrl_ = nullptr;
    wxSpinCtrlDouble*  presence_penalty_ctrl_  = nullptr;

    void on_preset_apply(wxCommandEvent& evt);
    void on_preset_choice(wxCommandEvent& evt);

    // Index controls
    wxTextCtrl*        exclude_ctrl_     = nullptr;

    // Semantic search controls
    wxCheckBox*        semantic_enabled_ctrl_ = nullptr;
    wxTextCtrl*        semantic_model_ctrl_   = nullptr;
    wxCheckBox*        reranker_enabled_ctrl_ = nullptr;
    wxTextCtrl*        reranker_model_ctrl_   = nullptr;
    wxSpinCtrl*        reranker_top_k_ctrl_   = nullptr;

    // S4.A / S5.Z -- workspace-level edit-safety toggle on the Tool Approvals
    // tab. When checked, edit_file refuses files that haven't been read in
    // this session. Default true.
    wxCheckBox*        require_read_before_edit_ctrl_ = nullptr;

    // Tool approval controls -- one choice per tool, same order as tool_names_.
    std::vector<std::string> tool_names_;
    std::vector<wxChoice*>   approval_choices_;
    // Snapshot of the choice index before the user's last change. Used by the
    // S4.V friction confirm to snap back when the user declines the prompt
    // after picking Auto-Approve on a mutating tool.
    std::vector<int>         approval_prev_sel_;

    // S4.V Task 4 -- friction confirm for mutating tools. Fired from the
    // wxEVT_CHOICE handler when the user picks "Auto-Approve" on a tool whose
    // name is in `is_mutating_tool()`. Returns true if the user confirmed and
    // the new selection should stick; false if the choice should snap back.
    bool confirm_auto_approve_mutating(const std::string& tool_name);

    // S5.A Capabilities tab.
    wxCheckBox*        cap_bg_      = nullptr;
    wxCheckBox*        cap_semantic_ = nullptr;
    wxCheckBox*        cap_code_    = nullptr;
    wxCheckBox*        cap_memory_  = nullptr;
    wxCheckBox*        cap_web_     = nullptr;

    // MCP tab controls (null when mcp_ == nullptr).
    wxListCtrl*        mcp_list_       = nullptr;   // server list
    wxStaticText*      mcp_detail_     = nullptr;   // last-error / status detail box
    wxCheckBox*        mcp_trust_      = nullptr;   // trust toggle for selected server
    wxButton*          mcp_restart_btn_ = nullptr;
    wxButton*          mcp_open_btn_    = nullptr;
    int                mcp_selected_   = -1;        // index into mcp_->status_snapshot()
    // Trust toggles for each server (`mcp:<name>:*` key), captured on init and
    // diffed on OK so we only persist changed entries.
    std::vector<std::pair<std::string, bool>> mcp_initial_trust_;
    std::vector<bool>                          mcp_current_trust_;
};

} // namespace locus
