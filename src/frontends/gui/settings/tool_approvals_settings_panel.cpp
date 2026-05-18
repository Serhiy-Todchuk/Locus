#include "tool_approvals_settings_panel.h"

#include "../locus_accessible.h"
#include "../ui_names.h"
#include "../../../core/global_config.h"
#include "../../../tools/permission_presets.h"
#include "../../../tools/tool.h"

#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>

namespace locus {

namespace {

bool is_mutating_tool(const std::string& name)
{
    return name == "edit_file"
        || name == "write_file"
        || name == "delete_file"
        || name == "run_command"
        || name == "run_command_bg";
}

} // namespace

ToolApprovalsSettingsPanel::ToolApprovalsSettingsPanel(wxWindow* parent,
                                                       const WorkspaceConfig& config,
                                                       IToolRegistry& tools)
    : wxPanel(parent)
    , tools_(tools)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // Capture wildcard / per-server MCP overrides at construction so they
    // round-trip through preset selection. Per-tool entries are managed by
    // the dropdown grid below.
    for (const auto& [key, val] : config.tool_approval_policies) {
        if (key.size() >= 2 && key.compare(key.size() - 2, 2, ":*") == 0) {
            wildcard_overrides_[key] = val;
        } else if (key.rfind("mcp:", 0) == 0) {
            wildcard_overrides_[key] = val;
        }
    }

    // -- S5.S preset row ---------------------------------------------------
    {
        auto* preset_row = new wxBoxSizer(wxHORIZONTAL);
        preset_row->Add(new wxStaticText(this, wxID_ANY, "Preset:"),
                        0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);

        wxArrayString preset_labels;
        for (auto p : tools::all_presets_in_order())
            preset_labels.Add(tools::display_name(p));

        preset_choice_ = new wxChoice(this, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, preset_labels);
        preset_choice_->SetName(ui_names::kSettingsApprovalsPresetChoice);
        gui::apply_locus_accessible_name(preset_choice_);
        preset_choice_->SetToolTip(
            "Pick a named preset to snapshot a full per-tool policy. "
            "After a preset is applied you can still tweak individual tools "
            "below; the chip will switch to \"Custom\".");
        preset_row->Add(preset_choice_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        preset_desc_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
        preset_row->Add(preset_desc_, 1,
                        wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

        outer->Add(preset_row, 0, wxEXPAND | wxALL, 8);

        preset_choice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            int sel = preset_choice_->GetSelection();
            auto order = tools::all_presets_in_order();
            if (sel < 0 || sel >= static_cast<int>(order.size())) return;
            tools::PermissionPreset preset = order[sel];

            // Selecting Custom is a no-op (no signature to apply). Snap the
            // dropdown back to whatever the current per-tool map detects as.
            if (preset == tools::PermissionPreset::custom) {
                refresh_preset_chip();
                return;
            }

            if (!confirm_preset_warning(preset)) {
                refresh_preset_chip();
                return;
            }

            apply_preset(preset);
        });
    }

    require_read_before_edit_ctrl_ = new wxCheckBox(this, wxID_ANY,
        "Require read_file before edit_file (default on)");
    require_read_before_edit_ctrl_->SetValue(config.require_read_before_edit);
    require_read_before_edit_ctrl_->SetToolTip(
        "When checked, edit_file refuses any path the agent hasn't read in "
        "this session via read_file. Cuts hallucinated edits on local "
        "models. Uncheck if you trust the loaded model to propose accurate "
        "old_string matches without confirming the file content first.");
    outer->Add(require_read_before_edit_ctrl_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    auto* scroll = new wxScrolledWindow(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxVSCROLL | wxBORDER_SIMPLE);
    scroll->SetScrollRate(0, 12);
    scroll->SetName(ui_names::kSettingsApprovalsList);
    gui::apply_locus_accessible_name(scroll);

    auto* scroll_sizer = new wxFlexGridSizer(2, wxSize(8, 4));
    scroll_sizer->AddGrowableCol(0, 1);

    std::vector<ITool*> sorted_tools = tools_.all();
    std::sort(sorted_tools.begin(), sorted_tools.end(),
              [](ITool* a, ITool* b) { return a->name() < b->name(); });

    wxArrayString policy_labels;
    policy_labels.Add(policy_display_name(ToolApprovalPolicy::ask));
    policy_labels.Add(policy_display_name(ToolApprovalPolicy::auto_approve));
    policy_labels.Add(policy_display_name(ToolApprovalPolicy::deny));

    tool_names_.reserve(sorted_tools.size());
    approval_choices_.reserve(sorted_tools.size());
    approval_prev_sel_.reserve(sorted_tools.size());

    for (auto* tool : sorted_tools) {
        const std::string& tname = tool->name();

        ToolApprovalPolicy effective = tool->approval_policy();
        auto it = config.tool_approval_policies.find(tname);
        if (it != config.tool_approval_policies.end())
            effective = it->second;

        auto* label = new wxStaticText(scroll, wxID_ANY, wxString::FromUTF8(tname));
        auto* choice = new wxChoice(scroll, wxID_ANY,
            wxDefaultPosition, wxDefaultSize, policy_labels);
        int sel = 0;
        switch (effective) {
            case ToolApprovalPolicy::ask:          sel = 0; break;
            case ToolApprovalPolicy::auto_approve: sel = 1; break;
            case ToolApprovalPolicy::deny:         sel = 2; break;
        }
        choice->SetSelection(sel);

        // Per-tool automation id: "locus.settings.approvals.choice.<tool_name>".
        // S5.L mutating-tool friction-confirm script addresses these by name.
        std::string aid = "locus.settings.approvals.choice." + tname;
        choice->SetName(wxString::FromUTF8(aid));
        gui::apply_locus_accessible_name(choice);

        scroll_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        scroll_sizer->Add(choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

        tool_names_.push_back(tname);
        approval_choices_.push_back(choice);
        approval_prev_sel_.push_back(sel);

        const size_t idx = approval_choices_.size() - 1;
        choice->Bind(wxEVT_CHOICE, [this, idx](wxCommandEvent&) {
            int new_sel = approval_choices_[idx]->GetSelection();
            if (new_sel == 1 /*Auto-Approve*/ &&
                approval_prev_sel_[idx] != 1 &&
                !confirm_auto_approve_mutating(tool_names_[idx]))
            {
                approval_choices_[idx]->SetSelection(approval_prev_sel_[idx]);
                return;
            }
            approval_prev_sel_[idx] = new_sel;
            refresh_preset_chip();
        });
    }

    scroll->SetSizer(scroll_sizer);
    scroll->FitInside();

    outer->Add(scroll, 1, wxEXPAND | wxALL, 8);

    // "Reset to global defaults" button.
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddStretchSpacer(1);
        auto* btn = new wxButton(this, wxID_ANY, "Reset to global defaults");
        btn->SetToolTip(
            "Reload the controls on this tab from ~/.locus/config.json. "
            "Other tabs are unaffected. Click OK to commit the new values, "
            "or Cancel to discard them.");
        btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            load_from_config(load_global_config_or_defaults());
        });
        row->Add(btn, 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    }

    SetSizer(outer);

    refresh_preset_chip();
}

bool ToolApprovalsSettingsPanel::confirm_auto_approve_mutating(const std::string& tool_name)
{
    if (!is_mutating_tool(tool_name)) return true;

    wxString msg = wxString::Format(
        "Auto-approve every '%s' call in this workspace?\n\n"
        "Future calls will execute without prompting. Locus will still\n"
        "record each call in the activity log and snapshot affected files\n"
        "so /undo can roll back the most recent turn.\n\n"
        "Revoke any time in Settings -> Tool Approvals.",
        wxString::FromUTF8(tool_name));

    wxMessageDialog dlg(this, msg, "Auto-approve mutating tool?",
                        wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    dlg.SetYesNoLabels("Auto-approve", "Keep asking");
    return dlg.ShowModal() == wxID_YES;
}

bool ToolApprovalsSettingsPanel::confirm_preset_warning(tools::PermissionPreset preset)
{
    if (preset == tools::PermissionPreset::read_only
        || preset == tools::PermissionPreset::ask_before_edits)
        return true;

    wxString msg;
    wxString title;
    long style = wxYES_NO | wxNO_DEFAULT | wxICON_WARNING;

    if (preset == tools::PermissionPreset::allow_edits) {
        title = "Apply 'Allow edits' preset?";
        msg   =
            "File CRUD (write_file, edit_file) will run without prompting.\n"
            "delete_file, shell commands (run_command / run_command_bg), and\n"
            "MCP tools will still ask. Checkpoints continue to snapshot every\n"
            "mutation so /undo works.\n\n"
            "Revoke by switching back to 'Ask before edits' at any time.";
    } else if (preset == tools::PermissionPreset::allow_all) {
        title = "Apply 'Allow all' preset?";
        msg   =
            "All file mutations AND shell commands will run without prompting.\n"
            "Combined with auto-compaction the agent can run unattended.\n\n"
            "MCP tools still ask -- elevate per server under Settings -> MCP\n"
            "if you really need fully-autonomous MCP calls.\n\n"
            "Continue?";
    } else {
        return true;
    }

    wxMessageDialog dlg(this, msg, title, style);
    dlg.SetYesNoLabels("Apply preset", "Cancel");
    return dlg.ShowModal() == wxID_YES;
}

void ToolApprovalsSettingsPanel::apply_preset(tools::PermissionPreset preset)
{
    auto signature = tools::preset_signature(preset, tools_);

    for (size_t i = 0; i < tool_names_.size(); ++i) {
        const std::string& tname = tool_names_[i];
        ITool* tool = tools_.find(tname);
        ToolApprovalPolicy default_policy =
            tool ? tool->approval_policy() : ToolApprovalPolicy::ask;

        ToolApprovalPolicy effective = default_policy;
        auto it = signature.find(tname);
        if (it != signature.end()) effective = it->second;

        int sel = 0;
        switch (effective) {
            case ToolApprovalPolicy::ask:          sel = 0; break;
            case ToolApprovalPolicy::auto_approve: sel = 1; break;
            case ToolApprovalPolicy::deny:         sel = 2; break;
        }
        approval_choices_[i]->SetSelection(sel);
        approval_prev_sel_[i] = sel;
    }

    // Force mcp:* to ask in wildcard_overrides_ so detect_preset is unambiguous.
    // Per-server overrides (mcp:<name>:tool) stay -- the MCP panel manages those.
    wildcard_overrides_["mcp:*"] = ToolApprovalPolicy::ask;

    refresh_preset_chip();
}

void ToolApprovalsSettingsPanel::refresh_preset_chip()
{
    if (!preset_choice_) return;

    // Compose the same map commit_to_config will produce -- including wildcard
    // overrides -- so the detection runs on the value that will actually persist.
    std::unordered_map<std::string, ToolApprovalPolicy> snap;
    for (size_t i = 0; i < tool_names_.size(); ++i) {
        int sel = approval_choices_[i]->GetSelection();
        ToolApprovalPolicy chosen = ToolApprovalPolicy::ask;
        if (sel == 1) chosen = ToolApprovalPolicy::auto_approve;
        else if (sel == 2) chosen = ToolApprovalPolicy::deny;

        ITool* tool = tools_.find(tool_names_[i]);
        ToolApprovalPolicy default_policy =
            tool ? tool->approval_policy() : ToolApprovalPolicy::ask;
        if (chosen != default_policy) snap[tool_names_[i]] = chosen;
    }
    for (const auto& [k, v] : wildcard_overrides_) snap[k] = v;

    tools::PermissionPreset detected = tools::detect_preset(snap, tools_);

    auto order = tools::all_presets_in_order();
    int idx = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == detected) { idx = static_cast<int>(i); break; }
    }
    preset_choice_->SetSelection(idx);
    if (preset_desc_)
        preset_desc_->SetLabel(tools::preset_description(detected));
}

void ToolApprovalsSettingsPanel::load_from_config(const WorkspaceConfig& cfg)
{
    if (require_read_before_edit_ctrl_)
        require_read_before_edit_ctrl_->SetValue(cfg.require_read_before_edit);

    // Refresh wildcard overrides from the reloaded config.
    wildcard_overrides_.clear();
    for (const auto& [key, val] : cfg.tool_approval_policies) {
        if (key.size() >= 2 && key.compare(key.size() - 2, 2, ":*") == 0) {
            wildcard_overrides_[key] = val;
        } else if (key.rfind("mcp:", 0) == 0) {
            wildcard_overrides_[key] = val;
        }
    }

    for (size_t i = 0; i < tool_names_.size(); ++i) {
        ITool* tool = tools_.find(tool_names_[i]);
        ToolApprovalPolicy effective = tool ? tool->approval_policy()
                                            : ToolApprovalPolicy::ask;
        auto it = cfg.tool_approval_policies.find(tool_names_[i]);
        if (it != cfg.tool_approval_policies.end()) effective = it->second;

        int sel = 0;
        switch (effective) {
            case ToolApprovalPolicy::ask:          sel = 0; break;
            case ToolApprovalPolicy::auto_approve: sel = 1; break;
            case ToolApprovalPolicy::deny:         sel = 2; break;
        }
        approval_choices_[i]->SetSelection(sel);
        approval_prev_sel_[i] = sel;
    }
    refresh_preset_chip();
}

bool ToolApprovalsSettingsPanel::validate(wxString& /*out_error*/) const
{
    return true;
}

void ToolApprovalsSettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    std::unordered_map<std::string, ToolApprovalPolicy> new_approvals;

    for (size_t i = 0; i < tool_names_.size(); ++i) {
        int sel = approval_choices_[i]->GetSelection();
        ToolApprovalPolicy chosen = ToolApprovalPolicy::ask;
        if (sel == 1) chosen = ToolApprovalPolicy::auto_approve;
        else if (sel == 2) chosen = ToolApprovalPolicy::deny;

        ITool* tool = tools_.find(tool_names_[i]);
        ToolApprovalPolicy default_policy = tool ? tool->approval_policy()
                                                 : ToolApprovalPolicy::ask;
        if (chosen != default_policy)
            new_approvals[tool_names_[i]] = chosen;
    }

    // Preserve all :* prefix entries already in cfg -- the MCP panel manages
    // those and will reconcile them in its own commit_to_config call.
    for (const auto& [key, val] : cfg.tool_approval_policies) {
        if (key.size() >= 2 && key.compare(key.size() - 2, 2, ":*") == 0)
            new_approvals[key] = val;
    }

    cfg.tool_approval_policies = std::move(new_approvals);

    if (require_read_before_edit_ctrl_)
        cfg.require_read_before_edit = require_read_before_edit_ctrl_->IsChecked();
}

} // namespace locus
