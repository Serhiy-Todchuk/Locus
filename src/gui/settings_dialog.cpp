#include "settings_dialog.h"

#include <spdlog/spdlog.h>

#include <sstream>

namespace locus {

SettingsDialog::SettingsDialog(wxWindow* parent, WorkspaceConfig& config)
    : wxDialog(parent, wxID_ANY, "Settings",
               wxDefaultPosition, wxSize(480, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , config_(config)
{
    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // -- LLM section ----------------------------------------------------------
    auto* llm_box = new wxStaticBoxSizer(wxVERTICAL, this, "LLM");
    auto* llm_grid = new wxFlexGridSizer(2, wxSize(8, 4));
    llm_grid->AddGrowableCol(1, 1);

    llm_grid->Add(new wxStaticText(this, wxID_ANY, "Endpoint URL:"),
                  0, wxALIGN_CENTER_VERTICAL);
    endpoint_ctrl_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(config.llm_endpoint));
    llm_grid->Add(endpoint_ctrl_, 1, wxEXPAND);

    llm_grid->Add(new wxStaticText(this, wxID_ANY, "Model:"),
                  0, wxALIGN_CENTER_VERTICAL);
    model_ctrl_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(config.llm_model));
    llm_grid->Add(model_ctrl_, 1, wxEXPAND);

    llm_grid->Add(new wxStaticText(this, wxID_ANY, "Temperature:"),
                  0, wxALIGN_CENTER_VERTICAL);
    temperature_ctrl_ = new wxSpinCtrlDouble(this, wxID_ANY);
    temperature_ctrl_->SetRange(0.0, 2.0);
    temperature_ctrl_->SetIncrement(0.1);
    temperature_ctrl_->SetDigits(2);
    temperature_ctrl_->SetValue(config.llm_temperature);
    llm_grid->Add(temperature_ctrl_, 0);

    llm_grid->Add(new wxStaticText(this, wxID_ANY, "Context limit:"),
                  0, wxALIGN_CENTER_VERTICAL);
    context_ctrl_ = new wxSpinCtrl(this, wxID_ANY);
    context_ctrl_->SetRange(0, 1048576);
    context_ctrl_->SetValue(config.llm_context_limit);
    llm_grid->Add(context_ctrl_, 0);

    auto* ctx_hint = new wxStaticText(this, wxID_ANY, "(0 = auto-detect from server)");
    ctx_hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    auto hint_font = ctx_hint->GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);
    ctx_hint->SetFont(hint_font);

    llm_box->Add(llm_grid, 0, wxEXPAND | wxALL, 4);
    llm_box->Add(ctx_hint, 0, wxLEFT | wxBOTTOM, 8);

    main_sizer->Add(llm_box, 0, wxEXPAND | wxALL, 8);

    // -- Index section --------------------------------------------------------
    auto* idx_box = new wxStaticBoxSizer(wxVERTICAL, this, "Index");

    idx_box->Add(new wxStaticText(this, wxID_ANY, "Exclude patterns (one per line):"),
                 0, wxLEFT | wxTOP, 4);

    // Join exclude patterns into newline-separated text.
    std::string patterns_text;
    for (size_t i = 0; i < config.exclude_patterns.size(); ++i) {
        if (i > 0) patterns_text += '\n';
        patterns_text += config.exclude_patterns[i];
    }
    exclude_ctrl_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(patterns_text),
        wxDefaultPosition, wxSize(-1, 100),
        wxTE_MULTILINE | wxHSCROLL);

    idx_box->Add(exclude_ctrl_, 1, wxEXPAND | wxALL, 4);
    main_sizer->Add(idx_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    // -- Buttons --------------------------------------------------------------
    auto* btn_sizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 8);

    SetSizer(main_sizer);

    Bind(wxEVT_BUTTON, &SettingsDialog::on_ok, this, wxID_OK);
}

void SettingsDialog::on_ok(wxCommandEvent& evt)
{
    std::string new_endpoint = endpoint_ctrl_->GetValue().ToStdString();
    std::string new_model    = model_ctrl_->GetValue().ToStdString();
    double      new_temp     = temperature_ctrl_->GetValue();
    int         new_context  = context_ctrl_->GetValue();

    // Parse exclude patterns from multiline text.
    std::vector<std::string> new_patterns;
    std::string text = exclude_ctrl_->GetValue().ToStdString();
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim whitespace.
        auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        auto end = line.find_last_not_of(" \t\r");
        new_patterns.push_back(line.substr(start, end - start + 1));
    }

    // Check what changed.
    if (new_endpoint != config_.llm_endpoint ||
        new_model    != config_.llm_model ||
        new_temp     != config_.llm_temperature ||
        new_context  != config_.llm_context_limit) {
        llm_changed_ = true;
    }

    if (new_patterns != config_.exclude_patterns) {
        index_changed_ = true;
    }

    changed_ = llm_changed_ || index_changed_;

    if (changed_) {
        config_.llm_endpoint      = new_endpoint;
        config_.llm_model         = new_model;
        config_.llm_temperature   = new_temp;
        config_.llm_context_limit = new_context;
        config_.exclude_patterns  = new_patterns;

        spdlog::info("Settings changed (llm={}, index={})", llm_changed_, index_changed_);
    }

    evt.Skip();  // let wxDialog handle EndModal(wxID_OK)
}

} // namespace locus
