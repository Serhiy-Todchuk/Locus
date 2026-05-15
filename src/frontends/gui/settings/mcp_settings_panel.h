#pragma once

#include "settings_panel.h"

#include <wx/listctrl.h>
#include <wx/wx.h>

#include <string>
#include <utility>
#include <vector>

namespace locus {

class McpManager;

// Settings tab: MCP server list, status display, restart, trust toggle,
// and Open mcp.json button. Null McpManager shows a hint-only fallback panel.
class McpSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    McpSettingsPanel(wxWindow* parent, const WorkspaceConfig& config,
                     McpManager* mcp);

    // load_from_config: "Reset" clears all trust toggles to off (global config
    // never carries mcp:* approval entries).
    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    // commit_to_config: removes managed mcp:<name>:* entries from cfg, then
    // adds back the ones the user toggled on. Unmanaged :* entries are untouched.
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    void refresh_mcp_list();
    void on_mcp_select(wxListEvent& evt);
    void on_mcp_restart(wxCommandEvent& evt);
    void on_mcp_open_json(wxCommandEvent& evt);

    McpManager* mcp_;

    wxListCtrl*   mcp_list_        = nullptr;
    wxStaticText* mcp_detail_      = nullptr;
    wxCheckBox*   mcp_trust_       = nullptr;
    wxButton*     mcp_restart_btn_ = nullptr;
    wxButton*     mcp_open_btn_    = nullptr;
    int           mcp_selected_    = -1;

    std::vector<std::pair<std::string, bool>> mcp_initial_trust_;
    std::vector<bool>                          mcp_current_trust_;
};

} // namespace locus
