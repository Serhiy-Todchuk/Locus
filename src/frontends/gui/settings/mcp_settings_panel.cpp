#include "mcp_settings_panel.h"

#include "../locus_accessible.h"
#include "../ui_names.h"
#include "../../../core/global_config.h"
#include "../../../mcp/mcp_client.h"
#include "../../../mcp/mcp_config.h"
#include "../../../mcp/mcp_manager.h"
#include "../../../tools/tool.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <filesystem>
#include <fstream>

namespace locus {

namespace {

constexpr int k_mcp_list_id    = wxID_HIGHEST + 2001;
constexpr int k_mcp_restart_id = wxID_HIGHEST + 2002;
constexpr int k_mcp_open_id    = wxID_HIGHEST + 2003;

wxString mcp_status_label(McpClient::Status s)
{
    switch (s) {
        case McpClient::Status::not_started:  return "not started";
        case McpClient::Status::initializing: return "initializing...";
        case McpClient::Status::ready:        return "ready";
        case McpClient::Status::failed:       return "failed";
        case McpClient::Status::crashed:      return "crashed";
        case McpClient::Status::stopped:      return "stopped";
    }
    return "?";
}

std::string trust_key(const std::string& server) { return "mcp:" + server + ":*"; }

} // namespace

McpSettingsPanel::McpSettingsPanel(wxWindow* parent, const WorkspaceConfig& config,
                                   McpManager* mcp)
    : wxPanel(parent)
    , mcp_(mcp)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);

    if (!mcp_) {
        auto* hint = new wxStaticText(this, wxID_ANY,
            "MCP support is initialised by LocusSession at startup. "
            "Create .locus/mcp.json (or %APPDATA%/Locus/mcp.json) and "
            "restart Locus to enable Model Context Protocol servers.\n\n"
            "Schema: architecture/mcp.md");
        outer->Add(hint, 0, wxALL, 12);
        SetSizer(outer);
        return;
    }

    auto* hint = new wxStaticText(this, wxID_ANY,
        "Servers loaded from .locus/mcp.json and %APPDATA%/Locus/mcp.json.\n"
        "Edit the file, then click Restart to re-spawn a server in place.");
    hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    hint->SetFont(hint_font);
    outer->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    mcp_list_ = new wxListCtrl(this, k_mcp_list_id,
        wxDefaultPosition, wxSize(-1, 160),
        wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    mcp_list_->AppendColumn("Server",  wxLIST_FORMAT_LEFT, 110);
    mcp_list_->AppendColumn("Status",  wxLIST_FORMAT_LEFT, 90);
    mcp_list_->AppendColumn("Tools",   wxLIST_FORMAT_LEFT, 60);
    mcp_list_->AppendColumn("Command", wxLIST_FORMAT_LEFT, 280);
    mcp_list_->SetName(ui_names::kSettingsMcpList);
    gui::apply_locus_accessible_name(mcp_list_);
    outer->Add(mcp_list_, 1, wxEXPAND | wxALL, 8);

    auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
    mcp_restart_btn_ = new wxButton(this, k_mcp_restart_id, "Restart");
    mcp_open_btn_    = new wxButton(this, k_mcp_open_id,    "Open mcp.json");
    mcp_trust_       = new wxCheckBox(this, wxID_ANY,
        "Trust selected server (auto-approve all mcp:<server>:* tools)");
    mcp_restart_btn_->SetName(ui_names::kSettingsMcpRestartBtn);
    mcp_open_btn_->SetName(ui_names::kSettingsMcpOpenJsonBtn);
    mcp_trust_->SetName(ui_names::kSettingsMcpTrustCheck);
    gui::apply_locus_accessible_name(mcp_restart_btn_);
    gui::apply_locus_accessible_name(mcp_open_btn_);
    gui::apply_locus_accessible_name(mcp_trust_);
    btn_row->Add(mcp_restart_btn_, 0, wxRIGHT, 6);
    btn_row->Add(mcp_open_btn_,    0, wxRIGHT, 18);
    btn_row->Add(mcp_trust_, 1, wxALIGN_CENTER_VERTICAL);
    outer->Add(btn_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    mcp_detail_ = new wxStaticText(this, wxID_ANY, "",
        wxDefaultPosition, wxSize(-1, 80), wxST_NO_AUTORESIZE);
    mcp_detail_->SetFont(hint_font);
    outer->Add(mcp_detail_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

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

    // Seed initial trust state so commit_to_config can diff against it.
    auto snap = mcp_->status_snapshot();
    mcp_initial_trust_.reserve(snap.size());
    mcp_current_trust_.reserve(snap.size());
    for (const auto& s : snap) {
        auto it = config.tool_approval_policies.find(trust_key(s.name));
        bool trusted = (it != config.tool_approval_policies.end() &&
                        it->second == ToolApprovalPolicy::auto_approve);
        mcp_initial_trust_.emplace_back(s.name, trusted);
        mcp_current_trust_.push_back(trusted);
    }

    Bind(wxEVT_BUTTON, &McpSettingsPanel::on_mcp_restart, this, k_mcp_restart_id);
    Bind(wxEVT_BUTTON, &McpSettingsPanel::on_mcp_open_json, this, k_mcp_open_id);
    Bind(wxEVT_LIST_ITEM_SELECTED, &McpSettingsPanel::on_mcp_select, this, k_mcp_list_id);

    mcp_trust_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
        if (mcp_selected_ < 0) return;
        mcp_current_trust_[mcp_selected_] = mcp_trust_->GetValue();
        e.Skip();
    });

    refresh_mcp_list();
    if (mcp_list_->GetItemCount() > 0) {
        mcp_list_->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    } else {
        mcp_restart_btn_->Disable();
        mcp_trust_->Disable();
        mcp_detail_->SetLabel("No MCP servers configured.");
    }
}

void McpSettingsPanel::refresh_mcp_list()
{
    if (!mcp_list_ || !mcp_) return;

    long preserve_sel = mcp_selected_;
    mcp_list_->DeleteAllItems();

    auto snap = mcp_->status_snapshot();
    for (size_t i = 0; i < snap.size(); ++i) {
        const auto& s = snap[i];
        long row = mcp_list_->InsertItem(static_cast<long>(i),
            wxString::FromUTF8(s.name));
        mcp_list_->SetItem(row, 1, mcp_status_label(s.status));
        mcp_list_->SetItem(row, 2,
            wxString::Format("%zu", s.tool_names.size()));
        mcp_list_->SetItem(row, 3, wxString::FromUTF8(s.command));
    }
    if (preserve_sel >= 0 && preserve_sel < mcp_list_->GetItemCount()) {
        mcp_list_->SetItemState(preserve_sel, wxLIST_STATE_SELECTED,
            wxLIST_STATE_SELECTED);
    }
}

void McpSettingsPanel::on_mcp_select(wxListEvent& evt)
{
    mcp_selected_ = static_cast<int>(evt.GetIndex());
    auto snap = mcp_->status_snapshot();
    if (mcp_selected_ < 0 || mcp_selected_ >= static_cast<int>(snap.size())) {
        mcp_detail_->SetLabel("");
        return;
    }
    const auto& s = snap[mcp_selected_];

    std::string detail = "Status: " + std::string(mcp_status_label(s.status).utf8_string());
    if (s.has_exit_code)
        detail += " (exit code " + std::to_string(s.exit_code) + ")";
    if (!s.tool_names.empty()) {
        detail += "\nTools: ";
        for (size_t i = 0; i < s.tool_names.size(); ++i) {
            if (i > 0) detail += ", ";
            detail += s.tool_names[i];
        }
    }
    if (!s.last_error.empty()) {
        detail += "\nLast error: " + s.last_error;
    }
    mcp_detail_->SetLabel(wxString::FromUTF8(detail));

    if (mcp_trust_)
        mcp_trust_->SetValue(mcp_current_trust_[mcp_selected_]);
}

void McpSettingsPanel::on_mcp_restart(wxCommandEvent&)
{
    if (!mcp_ || mcp_selected_ < 0) return;
    auto snap = mcp_->status_snapshot();
    if (mcp_selected_ >= static_cast<int>(snap.size())) return;

    const std::string name = snap[mcp_selected_].name;
    bool ok = mcp_->restart(name);
    refresh_mcp_list();
    if (mcp_selected_ < mcp_list_->GetItemCount()) {
        wxListEvent evt(wxEVT_LIST_ITEM_SELECTED, k_mcp_list_id);
        evt.m_itemIndex = mcp_selected_;
        on_mcp_select(evt);
    }
    if (!ok) {
        wxMessageBox(wxString::Format(
            "Failed to restart MCP server '%s'. See the activity log "
            "for details.", wxString::FromUTF8(name)),
            "MCP", wxOK | wxICON_WARNING, this);
    }
}

void McpSettingsPanel::on_mcp_open_json(wxCommandEvent&)
{
    if (!mcp_) return;
    auto path = McpConfigLoader::workspace_config_path(mcp_->workspace_root());

    if (!std::filesystem::exists(path)) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path);
        if (f.is_open()) {
            f << "{\n"
                 "  \"mcpServers\": {\n"
                 "  }\n"
                 "}\n";
        }
    }

    if (!wxLaunchDefaultApplication(wxString::FromUTF8(path.string()))) {
        wxMessageBox(wxString::Format("Could not open %s in the default editor.\n"
                                      "Path is on the clipboard if you want to open it manually.",
                                      wxString::FromUTF8(path.string())),
                     "MCP", wxOK | wxICON_INFORMATION, this);
    }
}

void McpSettingsPanel::load_from_config(const WorkspaceConfig& /*cfg*/)
{
    // Global config never carries mcp:* approval keys (save_global_config
    // strips them), so "reset to global defaults" for MCP = clear all trust
    // toggles to off.
    if (!mcp_) return;
    std::fill(mcp_current_trust_.begin(), mcp_current_trust_.end(), false);
    if (mcp_trust_ && mcp_selected_ >= 0 &&
        mcp_selected_ < static_cast<int>(mcp_current_trust_.size())) {
        mcp_trust_->SetValue(false);
    }
}

bool McpSettingsPanel::validate(wxString& /*out_error*/) const
{
    return true;
}

void McpSettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    if (!mcp_) return;

    // Remove the :* entries this panel manages, then re-apply current trust state.
    for (const auto& [name, _] : mcp_initial_trust_)
        cfg.tool_approval_policies.erase(trust_key(name));

    for (size_t i = 0; i < mcp_initial_trust_.size(); ++i) {
        if (mcp_current_trust_[i]) {
            cfg.tool_approval_policies[trust_key(mcp_initial_trust_[i].first)] =
                ToolApprovalPolicy::auto_approve;
        }
    }
}

} // namespace locus
