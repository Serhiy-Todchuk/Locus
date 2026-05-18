#include "notifications_settings_panel.h"

#include "../notification_sounds.h"
#include "../../../core/global_config.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace locus {

NotificationsSettingsPanel::NotificationsSettingsPanel(wxWindow* parent,
                                                       const WorkspaceConfig& config)
    : wxPanel(parent)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);

    auto* intro = new wxStaticText(this, wxID_ANY,
        "Play a Windows system sound when Locus needs your attention. "
        "Each event uses a distinct sound alias -- adjust the actual waveform "
        "in Control Panel -> Sound -> Sounds. Unchecking an event silences it "
        "without affecting the others.");
    intro->Wrap(560);
    outer->Add(intro, 0, wxALL, 8);

    auto add_row = [&](wxCheckBox*& cb, const wxString& label,
                       const wxString& sound_alias_label,
                       const wxString& hint_text,
                       bool initial)
    {
        wxString full = wxString::Format("%s  (%s)", label, sound_alias_label);
        cb = new wxCheckBox(this, wxID_ANY, full);
        cb->SetValue(initial);
        cb->SetToolTip(hint_text);
        outer->Add(cb, 0, wxLEFT | wxRIGHT | wxTOP, 8);

        auto* hint = new wxStaticText(this, wxID_ANY, hint_text);
        hint->SetFont(hint_font);
        hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        hint->Wrap(540);
        outer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 26);
    };

    add_row(snd_tool_approval_, "Tool approval pending", "Exclamation",
            "A tool call is waiting for your approve/reject decision.",
            config.notifications.sound_on_tool_approval);

    add_row(snd_ask_user_, "Ask-user question", "Question",
            "The agent invoked the ask_user tool to ask you a question.",
            config.notifications.sound_on_ask_user);

    add_row(snd_turn_complete_, "Work done (turn complete)", "Asterisk",
            "The agent has finished a turn and is idle again.",
            config.notifications.sound_on_turn_complete);

    add_row(snd_compaction_, "Compaction needed", "Critical Stop",
            "The context window is full and the compaction dialog has opened.",
            config.notifications.sound_on_compaction);

    outer->AddSpacer(8);

    only_unfocused_ = new wxCheckBox(this, wxID_ANY,
        "Only when the Locus window isn't focused");
    only_unfocused_->SetValue(config.notifications.only_when_unfocused);
    only_unfocused_->SetToolTip(
        "Skip the sound when Locus is already the foreground window. "
        "Keeps the alerts useful as 'come back to me' signals without "
        "chirping while you're actively watching the agent.");
    outer->Add(only_unfocused_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    // "Reset to global defaults" button -- mirrors the other settings tabs.
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

void NotificationsSettingsPanel::load_from_config(const WorkspaceConfig& cfg)
{
    if (snd_tool_approval_)
        snd_tool_approval_->SetValue(cfg.notifications.sound_on_tool_approval);
    if (snd_ask_user_)
        snd_ask_user_->SetValue(cfg.notifications.sound_on_ask_user);
    if (snd_turn_complete_)
        snd_turn_complete_->SetValue(cfg.notifications.sound_on_turn_complete);
    if (snd_compaction_)
        snd_compaction_->SetValue(cfg.notifications.sound_on_compaction);
    if (only_unfocused_)
        only_unfocused_->SetValue(cfg.notifications.only_when_unfocused);
}

bool NotificationsSettingsPanel::validate(wxString& /*out_error*/) const
{
    return true;
}

void NotificationsSettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    if (snd_tool_approval_)
        cfg.notifications.sound_on_tool_approval = snd_tool_approval_->IsChecked();
    if (snd_ask_user_)
        cfg.notifications.sound_on_ask_user = snd_ask_user_->IsChecked();
    if (snd_turn_complete_)
        cfg.notifications.sound_on_turn_complete = snd_turn_complete_->IsChecked();
    if (snd_compaction_)
        cfg.notifications.sound_on_compaction = snd_compaction_->IsChecked();
    if (only_unfocused_)
        cfg.notifications.only_when_unfocused = only_unfocused_->IsChecked();
}

} // namespace locus
