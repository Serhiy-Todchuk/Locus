#include "settings_dialog.h"

#include "locus_accessible.h"
#include "ui_names.h"
#include "settings/capabilities_settings_panel.h"
#include "settings/index_settings_panel.h"
#include "settings/llm_settings_panel.h"
#include "settings/mcp_settings_panel.h"
#include "settings/notifications_settings_panel.h"
#include "settings/sessions_settings_panel.h"
#include "settings/agent_settings_panel.h"
#include "settings/tool_approvals_settings_panel.h"
#include "../../core/global_config.h"
#include "../../core/global_paths.h"

#include <wx/sizer.h>
#include <spdlog/spdlog.h>

namespace locus {

SettingsDialog::SettingsDialog(wxWindow* parent, WorkspaceConfig& config,
                               IToolRegistry& tools, McpManager* mcp)
    : wxDialog(parent, wxID_ANY, "Settings",
               wxDefaultPosition, wxSize(620, 760),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , config_(config)
    , tools_(tools)
{
    SetName(ui_names::kSettingsDialog);
    gui::apply_locus_accessible_name(this);

    auto* notebook = new wxNotebook(this, wxID_ANY);
    notebook->SetName(ui_names::kSettingsNotebook);
    gui::apply_locus_accessible_name(notebook);

    llm_panel_           = new LlmSettingsPanel(notebook, config);
    index_panel_         = new IndexSettingsPanel(notebook, config);
    capabilities_panel_  = new CapabilitiesSettingsPanel(notebook, config);
    approvals_panel_     = new ToolApprovalsSettingsPanel(notebook, config, tools);
    mcp_panel_           = new McpSettingsPanel(notebook, config, mcp);
    notifications_panel_ = new NotificationsSettingsPanel(notebook, config);
    sessions_panel_      = new SessionsSettingsPanel(notebook, config);
    agent_panel_         = new AgentSettingsPanel(notebook, config);

    llm_panel_->SetName(ui_names::kSettingsTabLlm);
    index_panel_->SetName(ui_names::kSettingsTabIndex);
    capabilities_panel_->SetName(ui_names::kSettingsTabCapabilities);
    approvals_panel_->SetName(ui_names::kSettingsTabApprovals);
    mcp_panel_->SetName(ui_names::kSettingsTabMcp);
    notifications_panel_->SetName(ui_names::kSettingsTabNotifications);
    sessions_panel_->SetName(ui_names::kSettingsTabSessions);
    agent_panel_->SetName(ui_names::kSettingsTabAgent);
    gui::apply_locus_accessible_name(llm_panel_);
    gui::apply_locus_accessible_name(index_panel_);
    gui::apply_locus_accessible_name(capabilities_panel_);
    gui::apply_locus_accessible_name(approvals_panel_);
    gui::apply_locus_accessible_name(mcp_panel_);
    gui::apply_locus_accessible_name(notifications_panel_);
    gui::apply_locus_accessible_name(sessions_panel_);
    gui::apply_locus_accessible_name(agent_panel_);

    notebook->AddPage(llm_panel_,           "LLM");
    notebook->AddPage(index_panel_,         "Index");
    notebook->AddPage(agent_panel_,         "Agent");
    notebook->AddPage(capabilities_panel_,  "Capabilities");
    notebook->AddPage(approvals_panel_,     "Tool Approvals");
    notebook->AddPage(mcp_panel_,           "MCP Servers");
    notebook->AddPage(notifications_panel_, "Notifications");
    notebook->AddPage(sessions_panel_,      "Sessions");

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(notebook, 1, wxEXPAND | wxALL, 8);

    // S5.M -- bottom button row: [Save as global defaults]  [spacer]  [OK] [Cancel].
    auto* button_row = new wxBoxSizer(wxHORIZONTAL);
    auto* save_global_btn = new wxButton(this, wxID_ANY, "Save as global defaults");
    save_global_btn->SetToolTip(
        "Snapshot the current Settings values into ~/.locus/config.json. "
        "Future workspaces will use them as their starting template. "
        "Existing workspaces are not affected.");
    save_global_btn->Bind(wxEVT_BUTTON,
                          &SettingsDialog::on_save_as_global_defaults, this);
    save_global_btn->SetName(ui_names::kSettingsSaveAsGlobalBtn);
    gui::apply_locus_accessible_name(save_global_btn);
    button_row->Add(save_global_btn, 0, wxALIGN_CENTER_VERTICAL);
    button_row->AddStretchSpacer(1);
    button_row->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
                    wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(button_row, 0, wxEXPAND | wxALL, 8);

    SetSizer(main_sizer);

    Bind(wxEVT_BUTTON, &SettingsDialog::on_ok, this, wxID_OK);
}

void SettingsDialog::on_ok(wxCommandEvent& evt)
{
    // Validate all panels first.
    wxString err;
    if (!llm_panel_->validate(err)           ||
        !index_panel_->validate(err)         ||
        !capabilities_panel_->validate(err)  ||
        !approvals_panel_->validate(err)     ||
        !mcp_panel_->validate(err)           ||
        !notifications_panel_->validate(err) ||
        !sessions_panel_->validate(err)      ||
        !agent_panel_->validate(err))
    {
        wxMessageBox(err, "Settings", wxOK | wxICON_ERROR, this);
        return;
    }

    // Snapshot the baseline so we can detect what changed after commit.
    const WorkspaceConfig baseline = config_;

    // Build new_cfg from the current control state. Approvals must run before
    // MCP so the :* prefix keys written by MCP survive the approval rebuild.
    WorkspaceConfig new_cfg = config_;
    llm_panel_->commit_to_config(new_cfg);
    index_panel_->commit_to_config(new_cfg);
    capabilities_panel_->commit_to_config(new_cfg);
    approvals_panel_->commit_to_config(new_cfg);
    mcp_panel_->commit_to_config(new_cfg);
    notifications_panel_->commit_to_config(new_cfg);
    sessions_panel_->commit_to_config(new_cfg);
    agent_panel_->commit_to_config(new_cfg);

    // Cross-tab reconciliation: the Capabilities tab's semantic toggle is the
    // canonical authority. When the two differ, capabilities wins and we force
    // semantic_changed_ so the indexer reacts.
    if (new_cfg.capabilities.semantic_search != new_cfg.index.semantic_search_enabled) {
        new_cfg.index.semantic_search_enabled = new_cfg.capabilities.semantic_search;
    }

    // Determine which subsystems changed.
    llm_changed_ = (new_cfg.llm.endpoint       != baseline.llm.endpoint       ||
                    new_cfg.llm.model           != baseline.llm.model          ||
                    new_cfg.llm.temperature     != baseline.llm.temperature    ||
                    new_cfg.llm.context_limit   != baseline.llm.context_limit  ||
                    new_cfg.llm.max_tokens      != baseline.llm.max_tokens     ||
                    new_cfg.llm.tool_format     != baseline.llm.tool_format    ||
                    new_cfg.llm.top_p           != baseline.llm.top_p          ||
                    new_cfg.llm.top_k           != baseline.llm.top_k          ||
                    new_cfg.llm.min_p           != baseline.llm.min_p          ||
                    new_cfg.llm.repeat_penalty  != baseline.llm.repeat_penalty ||
                    new_cfg.llm.frequency_penalty != baseline.llm.frequency_penalty ||
                    new_cfg.llm.presence_penalty  != baseline.llm.presence_penalty);

    index_changed_ = (new_cfg.index.exclude_patterns != baseline.index.exclude_patterns);

    semantic_changed_ = (new_cfg.index.semantic_search_enabled != baseline.index.semantic_search_enabled ||
                         new_cfg.index.embedding_model         != baseline.index.embedding_model         ||
                         new_cfg.index.reranker_enabled        != baseline.index.reranker_enabled        ||
                         new_cfg.index.reranker_model          != baseline.index.reranker_model          ||
                         new_cfg.index.reranker_top_k          != baseline.index.reranker_top_k);

    bool approvals_changed = (new_cfg.tool_approval_policies != baseline.tool_approval_policies ||
                               new_cfg.agent.require_read_before_edit != baseline.agent.require_read_before_edit);

    capabilities_changed_ = (
        new_cfg.capabilities.background_processes != baseline.capabilities.background_processes ||
        new_cfg.capabilities.semantic_search      != baseline.capabilities.semantic_search      ||
        new_cfg.capabilities.code_aware_search    != baseline.capabilities.code_aware_search    ||
        new_cfg.capabilities.memory_bank          != baseline.capabilities.memory_bank          ||
        new_cfg.capabilities.web_retrieval        != baseline.capabilities.web_retrieval);

    changed_ = llm_changed_ || index_changed_ || semantic_changed_
            || approvals_changed || capabilities_changed_;

    if (changed_) {
        // Keep the legacy mirror in sync with the canonical capabilities bool.
        new_cfg.memory.enabled = new_cfg.capabilities.memory_bank;

        config_ = new_cfg;

        spdlog::info("Settings changed (llm={}, index={}, semantic={}, "
                     "approvals={}, capabilities={})",
                     llm_changed_, index_changed_, semantic_changed_,
                     approvals_changed, capabilities_changed_);
    }

    evt.Skip();
}

WorkspaceConfig SettingsDialog::snapshot_dialog_state() const
{
    // Start from the live workspace config so fields the dialog doesn't manage
    // (memory.*, agent.*, git.*) round-trip through the global file.
    WorkspaceConfig out = config_;

    llm_panel_->commit_to_config(out);
    index_panel_->commit_to_config(out);
    capabilities_panel_->commit_to_config(out);
    approvals_panel_->commit_to_config(out);
    notifications_panel_->commit_to_config(out);
    sessions_panel_->commit_to_config(out);
    agent_panel_->commit_to_config(out);
    // MCP panel deliberately excluded: trust keys are workspace-specific and
    // save_global_config filters them out anyway.

    // Keep legacy mirror in sync.
    out.memory.enabled = out.capabilities.memory_bank;

    return out;
}

void SettingsDialog::on_save_as_global_defaults(wxCommandEvent& /*evt*/)
{
    if (wxMessageBox(
            "Save the current Settings values as global defaults?\n\n"
            "These values will be used as the template for any new workspace "
            "you open from this point on. Existing workspaces are not affected.",
            "Save as global defaults",
            wxYES_NO | wxICON_QUESTION, this) != wxYES) {
        return;
    }

    GlobalConfig snap = snapshot_dialog_state();
    if (save_global_config(snap)) {
        wxMessageBox(
            wxString::Format("Saved to %s.",
                wxString::FromUTF8(global_paths::config_path().string())),
            "Locus", wxOK | wxICON_INFORMATION, this);
    } else {
        wxMessageBox("Failed to save global defaults. Check the log for details.",
                     "Locus", wxOK | wxICON_ERROR, this);
    }
}

} // namespace locus
