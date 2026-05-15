#pragma once

#include "../../core/workspace.h"

#include <wx/notebook.h>
#include <wx/wx.h>

namespace locus {

class IToolRegistry;
class McpManager;
class LlmSettingsPanel;
class IndexSettingsPanel;
class CapabilitiesSettingsPanel;
class ToolApprovalsSettingsPanel;
class McpSettingsPanel;

// Modal dialog for editing workspace + LLM settings.
// Thin shell: owns a wxNotebook and one per-tab ISettingsPanel. On OK, calls
// validate() then commit_to_config() on each panel in order.
//
// S4.G phase 2: five tabs -- LLM, Index, Capabilities, Tool Approvals, MCP.
// The MCP tab is populated only when a non-null McpManager is supplied;
// otherwise it shows a hint pointing the user at the mcp.json schema doc.
class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow* parent, WorkspaceConfig& config, IToolRegistry& tools,
                   McpManager* mcp = nullptr);

    // True if user clicked OK and values changed.
    bool config_changed()       const { return changed_; }

    // Which parts changed (for the caller to know what to reconfigure).
    bool llm_changed()          const { return llm_changed_; }
    bool index_changed()        const { return index_changed_; }
    bool semantic_changed()     const { return semantic_changed_; }
    bool capabilities_changed() const { return capabilities_changed_; }

private:
    void on_ok(wxCommandEvent& evt);
    void on_save_as_global_defaults(wxCommandEvent& evt);

    // Snapshots current dialog state into a fresh WorkspaceConfig for the
    // "Save as global defaults" flow. Calls commit_to_config on LLM, Index,
    // Capabilities, and Approvals panels (MCP trust keys excluded -- they are
    // workspace-specific and save_global_config filters them anyway).
    WorkspaceConfig snapshot_dialog_state() const;

    WorkspaceConfig& config_;
    IToolRegistry&   tools_;

    LlmSettingsPanel*           llm_panel_          = nullptr;
    IndexSettingsPanel*         index_panel_        = nullptr;
    CapabilitiesSettingsPanel*  capabilities_panel_ = nullptr;
    ToolApprovalsSettingsPanel* approvals_panel_    = nullptr;
    McpSettingsPanel*           mcp_panel_          = nullptr;

    bool changed_              = false;
    bool llm_changed_          = false;
    bool index_changed_        = false;
    bool semantic_changed_     = false;
    bool capabilities_changed_ = false;
};

} // namespace locus
