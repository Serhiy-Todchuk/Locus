#include "capabilities_settings_panel.h"

#include "../../../core/global_config.h"

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace locus {

CapabilitiesSettingsPanel::CapabilitiesSettingsPanel(wxWindow* parent,
                                                     const WorkspaceConfig& config)
    : wxPanel(parent)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);

    auto* intro = new wxStaticText(this, wxID_ANY,
        "Each bucket gates a family of tools that the LLM sees in its "
        "per-turn manifest. Disabling buckets you don't need keeps the "
        "manifest small -- valuable on short-context local models. "
        "Changes apply to the next agent turn.");
    intro->Wrap(560);
    outer->Add(intro, 0, wxALL, 8);

    auto add_row = [&](wxCheckBox*& cb, const wxString& label,
                       int token_estimate, const wxString& tooltip,
                       bool initial)
    {
        wxString full = wxString::Format("%s  (~%d prompt tokens)",
                                         label, token_estimate);
        cb = new wxCheckBox(this, wxID_ANY, full);
        cb->SetValue(initial);
        cb->SetToolTip(tooltip);
        outer->Add(cb, 0, wxLEFT | wxRIGHT | wxTOP, 8);

        auto* hint = new wxStaticText(this, wxID_ANY, tooltip);
        hint->SetFont(hint_font);
        hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        hint->Wrap(540);
        outer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 26);
    };

    add_row(cap_bg_, "Background processes",
            capability_token_estimates::k_background_processes,
            "Adds run_command_bg, read_process_output, stop_process, "
            "list_processes. Useful for dev servers and long-running jobs.",
            config.capabilities.background_processes);

    add_row(cap_semantic_, "Semantic search",
            capability_token_estimates::k_semantic_search,
            "Adds `semantic` and `hybrid` modes to the search tool, and "
            "loads the embedding model. Disabling here also disables the "
            "Index-tab semantic toggle.",
            config.capabilities.semantic_search);

    add_row(cap_code_, "Code-aware search",
            capability_token_estimates::k_code_aware_search,
            "Adds `symbols` and `ast` modes to the search tool, plus the "
            "get_file_outline tool. Skip for text-only workspaces.",
            config.capabilities.code_aware_search);

    add_row(cap_memory_, "Memory bank",
            capability_token_estimates::k_memory_bank,
            "Adds add_memory + search_memory tools, /memorize and /forget "
            "slash commands, and the system-prompt memory slot.",
            config.capabilities.memory_bank);

    add_row(cap_web_, "Web retrieval",
            capability_token_estimates::k_web_retrieval,
            "Adds web fetch + search tools when M6 web RAG ships. Currently "
            "a placeholder -- no tools register against this bucket yet.",
            config.capabilities.web_retrieval);

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
}

void CapabilitiesSettingsPanel::load_from_config(const WorkspaceConfig& cfg)
{
    if (cap_bg_)       cap_bg_->SetValue(cfg.capabilities.background_processes);
    if (cap_semantic_) cap_semantic_->SetValue(cfg.capabilities.semantic_search);
    if (cap_code_)     cap_code_->SetValue(cfg.capabilities.code_aware_search);
    if (cap_memory_)   cap_memory_->SetValue(cfg.capabilities.memory_bank);
    if (cap_web_)      cap_web_->SetValue(cfg.capabilities.web_retrieval);
}

bool CapabilitiesSettingsPanel::validate(wxString& /*out_error*/) const
{
    return true;
}

void CapabilitiesSettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    if (cap_bg_)       cfg.capabilities.background_processes = cap_bg_->IsChecked();
    if (cap_semantic_) cfg.capabilities.semantic_search      = cap_semantic_->IsChecked();
    if (cap_code_)     cfg.capabilities.code_aware_search    = cap_code_->IsChecked();
    if (cap_memory_)   cfg.capabilities.memory_bank          = cap_memory_->IsChecked();
    if (cap_web_)      cfg.capabilities.web_retrieval        = cap_web_->IsChecked();
}

} // namespace locus
