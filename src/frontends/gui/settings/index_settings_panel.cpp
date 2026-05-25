#include "index_settings_panel.h"

#include "../../../core/global_config.h"

#include <wx/sizer.h>
#include <wx/stattext.h>

#include <sstream>

namespace locus {

IndexSettingsPanel::IndexSettingsPanel(wxWindow* parent, const WorkspaceConfig& config)
    : wxPanel(parent)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    outer->Add(new wxStaticText(this, wxID_ANY, "Exclude patterns (one per line):"),
               0, wxLEFT | wxRIGHT | wxTOP, 8);

    std::string patterns_text;
    for (size_t i = 0; i < config.index.exclude_patterns.size(); ++i) {
        if (i > 0) patterns_text += '\n';
        patterns_text += config.index.exclude_patterns[i];
    }
    exclude_ctrl_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(patterns_text),
        wxDefaultPosition, wxSize(-1, 100),
        wxTE_MULTILINE | wxHSCROLL);
    outer->Add(exclude_ctrl_, 1, wxEXPAND | wxALL, 8);

    semantic_enabled_ctrl_ = new wxCheckBox(this, wxID_ANY, "Enable semantic search");
    semantic_enabled_ctrl_->SetValue(config.index.semantic_search_enabled);
    outer->Add(semantic_enabled_ctrl_, 0, wxLEFT | wxRIGHT, 8);

    auto* sem_grid = new wxFlexGridSizer(2, wxSize(8, 4));
    sem_grid->AddGrowableCol(1, 1);
    sem_grid->Add(new wxStaticText(this, wxID_ANY, "Semantic search model:"),
                  0, wxALIGN_CENTER_VERTICAL);
    semantic_model_ctrl_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(config.index.embedding_model));
    sem_grid->Add(semantic_model_ctrl_, 1, wxEXPAND);

    sem_grid->Add(new wxStaticText(this, wxID_ANY, "Reranker model:"),
                  0, wxALIGN_CENTER_VERTICAL);
    reranker_model_ctrl_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(config.index.reranker_model));
    sem_grid->Add(reranker_model_ctrl_, 1, wxEXPAND);

    sem_grid->Add(new wxStaticText(this, wxID_ANY, "Reranker candidate pool (top-K):"),
                  0, wxALIGN_CENTER_VERTICAL);
    reranker_top_k_ctrl_ = new wxSpinCtrl(this, wxID_ANY);
    reranker_top_k_ctrl_->SetRange(1, 200);
    reranker_top_k_ctrl_->SetValue(config.index.reranker_top_k);
    sem_grid->Add(reranker_top_k_ctrl_, 0);

    outer->Add(sem_grid, 0, wxEXPAND | wxALL, 8);

    reranker_enabled_ctrl_ = new wxCheckBox(this, wxID_ANY,
        "Enable cross-encoder reranker (slower, more accurate top-N)");
    reranker_enabled_ctrl_->SetValue(config.index.reranker_enabled);
    outer->Add(reranker_enabled_ctrl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

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

void IndexSettingsPanel::load_from_config(const WorkspaceConfig& cfg)
{
    if (exclude_ctrl_) {
        std::string text;
        for (size_t i = 0; i < cfg.index.exclude_patterns.size(); ++i) {
            if (i > 0) text += '\n';
            text += cfg.index.exclude_patterns[i];
        }
        exclude_ctrl_->ChangeValue(wxString::FromUTF8(text));
    }
    if (semantic_enabled_ctrl_) semantic_enabled_ctrl_->SetValue(cfg.index.semantic_search_enabled);
    if (semantic_model_ctrl_)   semantic_model_ctrl_->ChangeValue(wxString::FromUTF8(cfg.index.embedding_model));
    if (reranker_enabled_ctrl_) reranker_enabled_ctrl_->SetValue(cfg.index.reranker_enabled);
    if (reranker_model_ctrl_)   reranker_model_ctrl_->ChangeValue(wxString::FromUTF8(cfg.index.reranker_model));
    if (reranker_top_k_ctrl_)   reranker_top_k_ctrl_->SetValue(cfg.index.reranker_top_k);
}

bool IndexSettingsPanel::validate(wxString& /*out_error*/) const
{
    return true;
}

void IndexSettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    // Parse exclude patterns from multi-line text control.
    {
        std::vector<std::string> patterns;
        std::string text = exclude_ctrl_->GetValue().ToStdString();
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            auto start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos) continue;
            auto end = line.find_last_not_of(" \t\r");
            patterns.push_back(line.substr(start, end - start + 1));
        }
        cfg.index.exclude_patterns = std::move(patterns);
    }

    cfg.index.semantic_search_enabled = semantic_enabled_ctrl_->GetValue();
    cfg.index.embedding_model         = semantic_model_ctrl_->GetValue().ToStdString();
    cfg.index.reranker_enabled        = reranker_enabled_ctrl_->GetValue();
    cfg.index.reranker_model          = reranker_model_ctrl_->GetValue().ToStdString();
    cfg.index.reranker_top_k          = reranker_top_k_ctrl_->GetValue();
}

} // namespace locus
