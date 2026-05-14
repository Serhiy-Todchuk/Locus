#include "capabilities_dialog.h"

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace locus {

CapabilitiesDialog::CapabilitiesDialog(wxWindow* parent,
                                       WorkspaceConfig::Capabilities& caps)
    : wxDialog(parent, wxID_ANY, "Workspace Capabilities",
               wxDefaultPosition, wxSize(560, 460),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , caps_(caps)
    , initial_(caps)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* hint = new wxStaticText(this, wxID_ANY,
        "Choose which agent capabilities to enable for this workspace. "
        "Each bucket gates a family of tools that the LLM will see in its "
        "per-turn manifest. Disabling buckets you don't need keeps the "
        "manifest small -- valuable on short-context local models.\n\n"
        "Token estimates are static and shown next to each label. You can "
        "change these later in Settings -> Capabilities.");
    hint->Wrap(520);
    outer->Add(hint, 0, wxALL, 12);

    auto* grid = new wxBoxSizer(wxVERTICAL);

    build_row(grid, cb_bg_,
        "Background processes",
        capability_token_estimates::k_background_processes,
        "Adds run_command_bg, read_process_output, stop_process, "
        "list_processes -- the agent can start dev servers, watchers, "
        "and long-running jobs that survive across turns.",
        caps_.background_processes);

    build_row(grid, cb_semantic_,
        "Semantic search",
        capability_token_estimates::k_semantic_search,
        "Adds the `semantic` and `hybrid` modes to the search tool, "
        "and loads the embedding model into memory. Useful for "
        "natural-language code / doc retrieval; costs RAM + disk.",
        caps_.semantic_search);

    build_row(grid, cb_code_,
        "Code-aware search",
        capability_token_estimates::k_code_aware_search,
        "Adds the `symbols` and `ast` modes to the search tool, plus "
        "the get_file_outline tool. Skip if the workspace is text-only "
        "(wiki, docs archive, ZIM library).",
        caps_.code_aware_search);

    build_row(grid, cb_memory_,
        "Memory bank",
        capability_token_estimates::k_memory_bank,
        "Adds add_memory + search_memory tools, the /memorize and "
        "/forget slash commands, and a small system-prompt slot for "
        "pinned + recently-used entries. Skip for one-shot or "
        "single-session workspaces.",
        caps_.memory_bank);

    build_row(grid, cb_web_,
        "Web retrieval",
        capability_token_estimates::k_web_retrieval,
        "Adds web fetch + search tools when the M6 web RAG subsystem "
        "ships. Currently a placeholder -- no tools register against "
        "this bucket yet.",
        caps_.web_retrieval);

    outer->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    outer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL),
               0, wxEXPAND | wxALL, 12);

    SetSizer(outer);
    Layout();

    Bind(wxEVT_BUTTON, &CapabilitiesDialog::on_ok, this, wxID_OK);
}

void CapabilitiesDialog::build_row(wxSizer* parent, wxCheckBox*& ctrl,
                                    const wxString& label, int token_estimate,
                                    const wxString& tooltip, bool initial)
{
    wxString full_label = wxString::Format("%s  (~%d prompt tokens)",
                                           label, token_estimate);
    ctrl = new wxCheckBox(this, wxID_ANY, full_label);
    ctrl->SetValue(initial);
    ctrl->SetToolTip(tooltip);
    parent->Add(ctrl, 0, wxBOTTOM, 8);

    auto* hint = new wxStaticText(this, wxID_ANY, tooltip);
    auto font = hint->GetFont();
    font.SetPointSize(font.GetPointSize() - 1);
    hint->SetFont(font);
    hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    hint->Wrap(490);
    parent->Add(hint, 0, wxLEFT | wxBOTTOM, 22);
}

void CapabilitiesDialog::on_ok(wxCommandEvent& evt)
{
    caps_.background_processes = cb_bg_->IsChecked();
    caps_.semantic_search      = cb_semantic_->IsChecked();
    caps_.code_aware_search    = cb_code_->IsChecked();
    caps_.memory_bank          = cb_memory_->IsChecked();
    caps_.web_retrieval        = cb_web_->IsChecked();

    changed_ = caps_.background_processes != initial_.background_processes
            || caps_.semantic_search      != initial_.semantic_search
            || caps_.code_aware_search    != initial_.code_aware_search
            || caps_.memory_bank          != initial_.memory_bank
            || caps_.web_retrieval        != initial_.web_retrieval;

    evt.Skip();  // let wxDialog close with wxID_OK
}

} // namespace locus
