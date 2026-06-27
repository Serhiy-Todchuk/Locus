#include "security_settings_panel.h"

#include "../../../core/global_config.h"
#include "../ui_names.h"
#include "../locus_accessible.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/valnum.h>

#include <algorithm>

namespace locus {

SecuritySettingsPanel::SecuritySettingsPanel(wxWindow* parent,
                                             const WorkspaceConfig& config)
    : wxPanel(parent)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);
    auto grey = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

    auto add_hint = [&](const char* text) {
        auto* h = new wxStaticText(this, wxID_ANY, text);
        h->SetFont(hint_font);
        h->SetForegroundColour(grey);
        h->Wrap(540);
        outer->Add(h, 0, wxLEFT | wxRIGHT | wxBOTTOM, 26);
    };

    auto* intro = new wxStaticText(this, wxID_ANY,
        "Prompt-injection scanning over UNTRUSTED external ingress (web / MCP / "
        "ZIM). Workspace files are trusted and never scanned. The scanner is a "
        "transparency tripwire that wraps + flags suspicious text -- the approval "
        "gate is the real control.");
    intro->Wrap(560);
    outer->Add(intro, 0, wxALL, 8);

    // -- injection_scan ------------------------------------------------------
    injection_scan_ = new wxCheckBox(this, wxID_ANY,
        "Scan web + MCP ingress for injection patterns");
    injection_scan_->SetValue(config.security.injection_scan);
    injection_scan_->SetToolTip(
        "When on, text returned by web_fetch and MCP tools is scanned for "
        "prompt-injection patterns before it enters a tool result or the index. "
        "Findings wrap the body in a labelled fence and can escalate the tool's "
        "approval to ask.");
    injection_scan_->SetName(ui_names::kSettingsSecurityInjectionScan);
    gui::apply_locus_accessible_name(injection_scan_);
    outer->Add(injection_scan_, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    add_hint("Does not apply to workspace files (trusted by authorship). "
             "The ZIM origin stamp is unconditional regardless of this toggle.");

    // -- scan_zim ------------------------------------------------------------
    scan_zim_ = new wxCheckBox(this, wxID_ANY,
        "Also keyword-scan ZIM / Wikipedia content");
    scan_zim_->SetValue(config.security.scan_zim);
    scan_zim_->SetToolTip(
        "Opt-in. Encyclopedia text is overwhelmingly benign and a multi-GB "
        "scan is expensive, so this is off by default. The [wikipedia, "
        "untrusted] origin marker is shown either way.");
    scan_zim_->SetName(ui_names::kSettingsSecurityScanZim);
    gui::apply_locus_accessible_name(scan_zim_);
    outer->Add(scan_zim_, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    add_hint("Off by default. Only turn on if you fetched a ZIM from an "
             "untrusted source and want the keyword pass over its articles.");

    // -- block_confidence ----------------------------------------------------
    {
        outer->Add(new wxStaticText(this, wxID_ANY,
            "Escalate-to-ask confidence threshold (0.0 - 1.0):"),
            0, wxLEFT | wxRIGHT | wxTOP, 8);
        block_confidence_ = new wxTextCtrl(this, wxID_ANY,
            wxString::FromDouble(config.security.block_confidence, 2));
        block_confidence_->SetToolTip(
            "Findings at or above this confidence escalate the tool's approval "
            "to ask (Exfiltration escalates regardless). Lower = more cautious "
            "(more prompts); higher = quieter. Default 0.85.");
        block_confidence_->SetName(ui_names::kSettingsSecurityBlockConfidence);
        gui::apply_locus_accessible_name(block_confidence_);
        outer->Add(block_confidence_, 0, wxLEFT | wxRIGHT, 8);
        add_hint("Default 0.85. Below this, a finding only wraps the body; at or "
                 "above, it also forces the approval gate to ask.");
    }

    // -- max_scan_kb ---------------------------------------------------------
    {
        outer->Add(new wxStaticText(this, wxID_ANY,
            "Max KB scanned per ingress:"),
            0, wxLEFT | wxRIGHT | wxTOP, 8);
        max_scan_kb_ = new wxTextCtrl(this, wxID_ANY,
            wxString::Format("%d", config.security.max_scan_kb));
        max_scan_kb_->SetToolTip(
            "Cap on the bytes inspected per ingress; huge pages scan a head + "
            "tail window past this and the gap is noted in the banner. "
            "Default 256.");
        max_scan_kb_->SetName(ui_names::kSettingsSecurityMaxScanKb);
        gui::apply_locus_accessible_name(max_scan_kb_);
        outer->Add(max_scan_kb_, 0, wxLEFT | wxRIGHT, 8);
        add_hint("Default 256 KB. Larger covers more of a long page; the "
                 "payload-past-window evasion is documented, not airtight.");
    }

    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddStretchSpacer(1);
        auto* btn = new wxButton(this, wxID_ANY, "Reset to global defaults");
        btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            load_from_config(load_global_config_or_defaults());
        });
        row->Add(btn, 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, 8);
    }

    outer->AddStretchSpacer(1);
    SetSizer(outer);
}

void SecuritySettingsPanel::load_from_config(const WorkspaceConfig& cfg)
{
    if (injection_scan_)   injection_scan_->SetValue(cfg.security.injection_scan);
    if (scan_zim_)         scan_zim_->SetValue(cfg.security.scan_zim);
    if (block_confidence_)
        block_confidence_->SetValue(wxString::FromDouble(cfg.security.block_confidence, 2));
    if (max_scan_kb_)
        max_scan_kb_->SetValue(wxString::Format("%d", cfg.security.max_scan_kb));
}

bool SecuritySettingsPanel::validate(wxString& out_error) const
{
    if (block_confidence_) {
        double v = 0.0;
        if (!block_confidence_->GetValue().ToDouble(&v) || v < 0.0 || v > 1.0) {
            out_error = "Security: block confidence must be a number between "
                        "0.0 and 1.0.";
            return false;
        }
    }
    if (max_scan_kb_) {
        long v = 0;
        if (!max_scan_kb_->GetValue().ToLong(&v) || v < 1) {
            out_error = "Security: max scan KB must be a positive integer.";
            return false;
        }
    }
    return true;
}

void SecuritySettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    if (injection_scan_) cfg.security.injection_scan = injection_scan_->IsChecked();
    if (scan_zim_)       cfg.security.scan_zim       = scan_zim_->IsChecked();
    if (block_confidence_) {
        double v = 0.0;
        if (block_confidence_->GetValue().ToDouble(&v))
            cfg.security.block_confidence =
                static_cast<float>(std::clamp(v, 0.0, 1.0));
    }
    if (max_scan_kb_) {
        long v = 0;
        if (max_scan_kb_->GetValue().ToLong(&v) && v >= 1)
            cfg.security.max_scan_kb = static_cast<int>(v);
    }
}

} // namespace locus
