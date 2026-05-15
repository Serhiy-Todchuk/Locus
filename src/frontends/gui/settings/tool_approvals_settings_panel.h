#pragma once

#include "settings_panel.h"

#include <wx/scrolwin.h>
#include <wx/wx.h>

#include <string>
#include <vector>

namespace locus {

class IToolRegistry;

// Settings tab: per-tool Ask/Auto/Deny dropdowns + read-before-edit toggle.
// Implements the S4.V Task 4 friction confirm for mutating-tool auto-approve.
class ToolApprovalsSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    ToolApprovalsSettingsPanel(wxWindow* parent, const WorkspaceConfig& config,
                               IToolRegistry& tools);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    bool confirm_auto_approve_mutating(const std::string& tool_name);

    IToolRegistry& tools_;

    wxCheckBox*               require_read_before_edit_ctrl_ = nullptr;
    std::vector<std::string>  tool_names_;
    std::vector<wxChoice*>    approval_choices_;
    std::vector<int>          approval_prev_sel_;
};

} // namespace locus
