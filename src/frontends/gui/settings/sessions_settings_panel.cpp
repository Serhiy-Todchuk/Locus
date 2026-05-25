#include "sessions_settings_panel.h"

#include "../../../core/global_config.h"
#include "../ui_names.h"
#include "../locus_accessible.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace locus {

SessionsSettingsPanel::SessionsSettingsPanel(wxWindow* parent,
                                             const WorkspaceConfig& config)
    : wxPanel(parent)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);

    auto* intro = new wxStaticText(this, wxID_ANY,
        "Per-workspace session behaviour. Cleanup thresholds "
        "(keep_last_count / delete_after_days) live in .locus/config.json "
        "until a dedicated UI lands.");
    intro->Wrap(560);
    outer->Add(intro, 0, wxALL, 8);

    restore_last_ = new wxCheckBox(this, wxID_ANY,
        "Restore open tabs from the last session on workspace open");
    restore_last_->SetValue(config.sessions.restore_last);
    restore_last_->SetToolTip(
        "When on, every tab open at last close is re-opened next time you "
        "open this workspace. When off, the workspace opens with a single "
        "empty tab.");
    restore_last_->SetName(ui_names::kSettingsSessionsRestoreLast);
    gui::apply_locus_accessible_name(restore_last_);
    outer->Add(restore_last_, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    {
        auto* hint = new wxStaticText(this, wxID_ANY,
            "Tabs are tracked in .locus/ui_state.json. Disabling here does "
            "NOT clear that file -- it just stops the auto-restore on open.");
        hint->SetFont(hint_font);
        hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        hint->Wrap(540);
        outer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 26);
    }

    persist_activity_ = new wxCheckBox(this, wxID_ANY,
        "Persist activity log across restarts");
    persist_activity_->SetValue(config.sessions.persist_activity);
    persist_activity_->SetToolTip(
        "Write every activity event to a JSONL sidecar file alongside the "
        "session JSON. Replayed on load so the Activity panel shows the "
        "full history of past turns. Adds a small per-event disk write; "
        "default off.");
    persist_activity_->SetName(ui_names::kSettingsSessionsPersistActivity);
    gui::apply_locus_accessible_name(persist_activity_);
    outer->Add(persist_activity_, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    {
        auto* hint = new wxStaticText(this, wxID_ANY,
            "Sidecar lives at .locus/sessions/<id>.activity.jsonl and is "
            "removed alongside the session JSON when you delete a session.");
        hint->SetFont(hint_font);
        hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        hint->Wrap(540);
        outer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 26);
    }

    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddStretchSpacer(1);
        auto* btn = new wxButton(this, wxID_ANY, "Reset to global defaults");
        btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            load_from_config(load_global_config_or_defaults());
        });
        row->Add(btn, 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    }

    SetSizer(outer);
}

void SessionsSettingsPanel::load_from_config(const WorkspaceConfig& cfg)
{
    if (restore_last_)     restore_last_->SetValue(cfg.sessions.restore_last);
    if (persist_activity_) persist_activity_->SetValue(cfg.sessions.persist_activity);
}

bool SessionsSettingsPanel::validate(wxString& /*out_error*/) const
{
    return true;
}

void SessionsSettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    if (restore_last_)
        cfg.sessions.restore_last = restore_last_->IsChecked();
    if (persist_activity_)
        cfg.sessions.persist_activity = persist_activity_->IsChecked();
}

} // namespace locus
