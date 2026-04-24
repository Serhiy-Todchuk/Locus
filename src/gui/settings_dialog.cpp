#include "settings_dialog.h"

#include "../tool.h"

#include <wx/scrolwin.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>

namespace locus {

SettingsDialog::SettingsDialog(wxWindow* parent, WorkspaceConfig& config,
                               IToolRegistry& tools)
    : wxDialog(parent, wxID_ANY, "Settings",
               wxDefaultPosition, wxSize(560, 640),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , config_(config)
    , tools_(tools)
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

    semantic_enabled_ctrl_ = new wxCheckBox(this, wxID_ANY, "Enable semantic search");
    semantic_enabled_ctrl_->SetValue(config.semantic_search_enabled);
    idx_box->Add(semantic_enabled_ctrl_, 0, wxALL, 4);

    auto* sem_grid = new wxFlexGridSizer(2, wxSize(8, 4));
    sem_grid->AddGrowableCol(1, 1);
    sem_grid->Add(new wxStaticText(this, wxID_ANY, "Semantic search model:"),
                  0, wxALIGN_CENTER_VERTICAL);
    semantic_model_ctrl_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(config.embedding_model));
    sem_grid->Add(semantic_model_ctrl_, 1, wxEXPAND);

    sem_grid->Add(new wxStaticText(this, wxID_ANY, "Reranker model:"),
                  0, wxALIGN_CENTER_VERTICAL);
    reranker_model_ctrl_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(config.reranker_model));
    sem_grid->Add(reranker_model_ctrl_, 1, wxEXPAND);

    sem_grid->Add(new wxStaticText(this, wxID_ANY, "Reranker candidate pool (top-K):"),
                  0, wxALIGN_CENTER_VERTICAL);
    reranker_top_k_ctrl_ = new wxSpinCtrl(this, wxID_ANY);
    reranker_top_k_ctrl_->SetRange(1, 200);
    reranker_top_k_ctrl_->SetValue(config.reranker_top_k);
    sem_grid->Add(reranker_top_k_ctrl_, 0);

    idx_box->Add(sem_grid, 0, wxEXPAND | wxALL, 4);

    reranker_enabled_ctrl_ = new wxCheckBox(this, wxID_ANY,
        "Enable cross-encoder reranker (slower, more accurate top-N)");
    reranker_enabled_ctrl_->SetValue(config.reranker_enabled);
    idx_box->Add(reranker_enabled_ctrl_, 0, wxALL, 4);

    main_sizer->Add(idx_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    // -- Tool Approvals section ----------------------------------------------
    auto* approval_box = new wxStaticBoxSizer(wxVERTICAL, this, "Tool Approvals");

    // Scrolled inner window so long tool lists don't blow out the dialog.
    auto* scroll = new wxScrolledWindow(this, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 180),
        wxVSCROLL | wxBORDER_SIMPLE);
    scroll->SetScrollRate(0, 12);

    auto* scroll_sizer = new wxFlexGridSizer(2, wxSize(8, 4));
    scroll_sizer->AddGrowableCol(0, 1);

    // Collect tools in a stable alphabetical order so the UI doesn't shuffle.
    std::vector<ITool*> sorted_tools = tools.all();
    std::sort(sorted_tools.begin(), sorted_tools.end(),
              [](ITool* a, ITool* b) { return a->name() < b->name(); });

    wxArrayString policy_labels;
    policy_labels.Add(policy_display_name(ToolApprovalPolicy::ask));
    policy_labels.Add(policy_display_name(ToolApprovalPolicy::auto_approve));
    policy_labels.Add(policy_display_name(ToolApprovalPolicy::deny));

    tool_names_.reserve(sorted_tools.size());
    approval_choices_.reserve(sorted_tools.size());

    for (auto* tool : sorted_tools) {
        const std::string& tname = tool->name();

        // Resolve effective starting policy: override if present, else default.
        ToolApprovalPolicy effective = tool->approval_policy();
        auto it = config.tool_approval_policies.find(tname);
        if (it != config.tool_approval_policies.end())
            effective = it->second;

        auto* label = new wxStaticText(scroll, wxID_ANY, wxString::FromUTF8(tname));
        auto* choice = new wxChoice(scroll, wxID_ANY,
            wxDefaultPosition, wxDefaultSize, policy_labels);
        int sel = 0;
        switch (effective) {
            case ToolApprovalPolicy::ask:          sel = 0; break;
            case ToolApprovalPolicy::auto_approve: sel = 1; break;
            case ToolApprovalPolicy::deny:         sel = 2; break;
        }
        choice->SetSelection(sel);

        scroll_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        scroll_sizer->Add(choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

        tool_names_.push_back(tname);
        approval_choices_.push_back(choice);
    }

    scroll->SetSizer(scroll_sizer);
    scroll->FitInside();

    approval_box->Add(scroll, 1, wxEXPAND | wxALL, 4);
    main_sizer->Add(approval_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

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

    bool new_semantic = semantic_enabled_ctrl_->GetValue();
    std::string new_sem_model = semantic_model_ctrl_->GetValue().ToStdString();
    bool new_reranker_enabled = reranker_enabled_ctrl_->GetValue();
    std::string new_reranker_model = reranker_model_ctrl_->GetValue().ToStdString();
    int new_reranker_top_k = reranker_top_k_ctrl_->GetValue();

    if (new_patterns != config_.exclude_patterns)
        index_changed_ = true;
    if (new_semantic != config_.semantic_search_enabled ||
        new_sem_model != config_.embedding_model ||
        new_reranker_enabled != config_.reranker_enabled ||
        new_reranker_model != config_.reranker_model ||
        new_reranker_top_k != config_.reranker_top_k)
        semantic_changed_ = true;

    // Collect new tool approval overrides. Only persist entries that
    // differ from the tool's built-in default — keeps config.json tidy.
    std::unordered_map<std::string, ToolApprovalPolicy> new_approvals;
    for (size_t i = 0; i < tool_names_.size(); ++i) {
        int sel = approval_choices_[i]->GetSelection();
        ToolApprovalPolicy chosen = ToolApprovalPolicy::ask;
        if (sel == 1) chosen = ToolApprovalPolicy::auto_approve;
        else if (sel == 2) chosen = ToolApprovalPolicy::deny;

        ITool* tool = tools_.find(tool_names_[i]);
        ToolApprovalPolicy default_policy = tool ? tool->approval_policy()
                                                 : ToolApprovalPolicy::ask;
        if (chosen != default_policy)
            new_approvals[tool_names_[i]] = chosen;
    }

    bool approvals_changed = (new_approvals != config_.tool_approval_policies);

    changed_ = llm_changed_ || index_changed_ || semantic_changed_ || approvals_changed;

    if (changed_) {
        config_.llm_endpoint      = new_endpoint;
        config_.llm_model         = new_model;
        config_.llm_temperature   = new_temp;
        config_.llm_context_limit = new_context;
        config_.exclude_patterns  = new_patterns;
        config_.semantic_search_enabled = new_semantic;
        config_.embedding_model   = new_sem_model;
        config_.reranker_enabled  = new_reranker_enabled;
        config_.reranker_model    = new_reranker_model;
        config_.reranker_top_k    = new_reranker_top_k;
        config_.tool_approval_policies = std::move(new_approvals);

        spdlog::info("Settings changed (llm={}, index={}, semantic={}, approvals={})",
                     llm_changed_, index_changed_, semantic_changed_, approvals_changed);
    }

    evt.Skip();  // let wxDialog handle EndModal(wxID_OK)
}

} // namespace locus
