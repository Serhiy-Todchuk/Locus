#pragma once

#include "settings_panel.h"
#include "../../../tools/permission_presets.h"

#include <wx/choice.h>
#include <wx/scrolwin.h>
#include <wx/spinctrl.h>
#include <wx/wx.h>

#include <string>
#include <vector>

namespace locus {

class IToolRegistry;

// Settings tab: per-tool Ask/Auto/Deny dropdowns + read-before-edit toggle.
// Implements the S4.V Task 4 friction confirm for mutating-tool auto-approve.
//
// S5.S -- adds a "Preset:" dropdown above the per-tool list. Selecting a named
// preset writes the full per-tool snapshot into the panel's working copy and
// re-paints every dropdown row to match. Per-tool tweaks afterward flip
// detection to "Custom" (label updates lazily on next dropdown toggle).
class ToolApprovalsSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    ToolApprovalsSettingsPanel(wxWindow* parent, const WorkspaceConfig& config,
                               IToolRegistry& tools);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    bool confirm_auto_approve_mutating(const std::string& tool_name);
    bool confirm_preset_warning(tools::PermissionPreset preset);

    // Recompute the current overrides from the live dropdowns + persisted
    // mcp:* entries, then update the preset chip to match.
    void refresh_preset_chip();

    // Apply a named preset's signature to the panel's working copy, repaint
    // each per-tool dropdown, and refresh the chip.
    void apply_preset(tools::PermissionPreset preset);

    IToolRegistry& tools_;

    // S5.S preset row.
    wxChoice*     preset_choice_  = nullptr;
    wxStaticText* preset_desc_    = nullptr;
    // Sparse :* / mcp:<server>:* overrides that the per-tool list doesn't
    // surface. Preserved across preset selection so we don't clobber MCP trust.
    std::unordered_map<std::string, ToolApprovalPolicy> wildcard_overrides_;

    wxCheckBox*               require_read_before_edit_ctrl_ = nullptr;
    wxSpinCtrl*               truncate_lines_ctrl_           = nullptr;
    // S6.11 -- lazy tool manifest checkbox.
    wxCheckBox*               lazy_tool_manifest_ctrl_       = nullptr;
    // S6.12 -- system-prompt profile dropdown.
    wxChoice*                 system_prompt_profile_ctrl_    = nullptr;
    std::vector<std::string>  tool_names_;
    std::vector<wxChoice*>    approval_choices_;
    std::vector<int>          approval_prev_sel_;
};

} // namespace locus
