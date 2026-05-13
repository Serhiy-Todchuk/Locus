#pragma once

#include "../../tools/tool.h"
#include "../../core/workspace.h"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>

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

    void on_ok(wxCommandEvent& evt);
    void refresh_mcp_list();
    void on_mcp_restart(wxCommandEvent& evt);
    void on_mcp_open_json(wxCommandEvent& evt);
    void on_mcp_select(wxListEvent& evt);

    WorkspaceConfig& config_;
    IToolRegistry&   tools_;
    McpManager*      mcp_;

    bool changed_        = false;
    bool llm_changed_    = false;
    bool index_changed_  = false;
    bool semantic_changed_ = false;

    // LLM controls
    wxTextCtrl*        endpoint_ctrl_    = nullptr;
    wxTextCtrl*        model_ctrl_       = nullptr;
    wxSpinCtrlDouble*  temperature_ctrl_ = nullptr;
    wxSpinCtrl*        context_ctrl_     = nullptr;
    wxSpinCtrl*        max_tokens_ctrl_  = nullptr;

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
    // Snapshot of the choice index before the user's last change. Used by the
    // S4.V friction confirm to snap back when the user declines the prompt
    // after picking Auto-Approve on a mutating tool.
    std::vector<int>         approval_prev_sel_;

    // S4.V Task 4 -- friction confirm for mutating tools. Fired from the
    // wxEVT_CHOICE handler when the user picks "Auto-Approve" on a tool whose
    // name is in `is_mutating_tool()`. Returns true if the user confirmed and
    // the new selection should stick; false if the choice should snap back.
    bool confirm_auto_approve_mutating(const std::string& tool_name);

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
