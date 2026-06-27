#include "about_dialog.h"

#include "app_icons.h"
#include "locus_accessible.h"
#include "ui_names.h"

#include <wx/statbmp.h>
#include <wx/statline.h>

namespace locus {
namespace {

constexpr const char* k_version = "0.1.0";
constexpr int         k_wrap_width = 610;

wxStaticText* wrapped_text(wxWindow* parent, const wxString& text)
{
    auto* st = new wxStaticText(parent, wxID_ANY, text);
    st->Wrap(k_wrap_width);
    return st;
}

wxStaticText* section_title(wxWindow* parent, const wxString& text)
{
    auto* st = new wxStaticText(parent, wxID_ANY, text);
    auto font = st->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    st->SetFont(font);
    return st;
}

void add_section(wxWindow* parent, wxSizer* sizer,
                 const wxString& title, const wxString& body)
{
    auto* box = new wxStaticBoxSizer(wxVERTICAL, parent, title);
    box->Add(wrapped_text(parent, body), 0, wxEXPAND | wxALL, 8);
    sizer->Add(box, 0, wxEXPAND | wxBOTTOM, 10);
}

} // namespace

AboutDialog::AboutDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "About Locus",
               wxDefaultPosition, wxSize(760, 640),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetName(ui_names::kAboutDialog);
    SetIcon(gui::app_icon(32));
    gui::apply_locus_accessible_name(this);

    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(new wxStaticBitmap(this, wxID_ANY,
                   wxBitmap(gui::app_icon(64))),
                0, wxRIGHT | wxALIGN_TOP, 14);

    auto* title_col = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(this, wxID_ANY, "Locus");
    auto title_font = title->GetFont();
    title_font.SetPointSize(title_font.GetPointSize() + 7);
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(title_font);
    title_col->Add(title, 0, wxBOTTOM, 2);
    title_col->Add(new wxStaticText(this, wxID_ANY,
                   wxString::Format("Version %s", k_version)),
                   0, wxBOTTOM, 8);
    title_col->Add(wrapped_text(this,
        "A local-first AI agent for your workspace: code, documents, "
        "sessions, tools, and model context kept visible and under your "
        "control."),
        0, wxEXPAND);
    header->Add(title_col, 1, wxEXPAND);
    outer->Add(header, 0, wxEXPAND | wxALL, 16);
    outer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 16);

    auto* content = new wxBoxSizer(wxVERTICAL);
    content->Add(section_title(this, "What Locus does"),
                 0, wxBOTTOM, 6);
    content->Add(wrapped_text(this,
        "Locus indexes the open workspace, connects to an OpenAI-compatible "
        "local LLM server, and gives the model focused tools for search, "
        "editing, shell work, memory, MCP integrations, and session history. "
        "Tool calls, approvals, diffs, context usage, progress, and undo "
        "points are surfaced in the desktop app so the agent remains "
        "inspectable while it works."),
        0, wxEXPAND | wxBOTTOM, 10);

    add_section(this, content, "Local-first by design",
        "Workspace indexes, embeddings, memories, sessions, checkpoints, "
        "and logs are stored on this machine. Locus only talks to the LLM "
        "endpoint and integrations you configure.");

    add_section(this, content, "Built for local models",
        "Retrieval, semantic and hybrid search, reranking, lean tool "
        "manifests, context budgeting, and compaction help smaller local "
        "models spend less of the prompt guessing and more of it using the "
        "right evidence.");

    add_section(this, content, "Transparent and tunable",
        "The point is that you understand what the agent is doing and can "
        "shape how it works, not that you approve every step. Inline diffs, "
        "activity events, terminal output, the system-prompt and context "
        "breakdown, saved sessions, and undo checkpoints keep the workflow "
        "legible. Capability toggles, permission presets, prompt-cost and "
        "compaction profiles, and per-model presets let you trade autonomy "
        "for oversight and tune efficiency to the model you are running.");

    add_section(this, content, "Platform and license",
        "Locus is currently developed and tested on Windows 11. The core is "
        "written with cross-platform support in mind, while several runtime "
        "integrations remain Windows-first. Developer: Serhiy Todchuk, 2026. "
        "License: MIT.");

    outer->Add(content, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    outer->Add(CreateStdDialogButtonSizer(wxOK), 0,
               wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, 16);

    SetSizer(outer);
    Layout();
    outer->SetSizeHints(this);
    Fit();
    CentreOnParent();
}

} // namespace locus
