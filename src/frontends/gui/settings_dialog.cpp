#include "settings_dialog.h"

#include "../../mcp/mcp_config.h"
#include "../../mcp/mcp_manager.h"
#include "../../tools/tool.h"

#include <wx/filename.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <spdlog/spdlog.h>

#include <fstream>

#include <algorithm>
#include <sstream>

namespace locus {

namespace {

constexpr int k_mcp_list_id     = wxID_HIGHEST + 1001;
constexpr int k_mcp_restart_id  = wxID_HIGHEST + 1002;
constexpr int k_mcp_open_id     = wxID_HIGHEST + 1003;

wxString mcp_status_label(McpClient::Status s)
{
    switch (s) {
        case McpClient::Status::not_started:  return "not started";
        case McpClient::Status::initializing: return "initializing...";
        case McpClient::Status::ready:        return "ready";
        case McpClient::Status::failed:       return "failed";
        case McpClient::Status::crashed:      return "crashed";
        case McpClient::Status::stopped:      return "stopped";
    }
    return "?";
}

std::string trust_key(const std::string& server) { return "mcp:" + server + ":*"; }

} // namespace

SettingsDialog::SettingsDialog(wxWindow* parent, WorkspaceConfig& config,
                               IToolRegistry& tools, McpManager* mcp)
    : wxDialog(parent, wxID_ANY, "Settings",
               wxDefaultPosition, wxSize(620, 560),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , config_(config)
    , tools_(tools)
    , mcp_(mcp)
{
    auto* notebook = new wxNotebook(this, wxID_ANY);
    notebook->AddPage(build_llm_tab(),       "LLM");
    notebook->AddPage(build_index_tab(),     "Index");
    notebook->AddPage(build_approvals_tab(), "Tool Approvals");
    notebook->AddPage(build_mcp_tab(),       "MCP Servers");

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(notebook, 1, wxEXPAND | wxALL, 8);
    main_sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL),
                    0, wxEXPAND | wxALL, 8);

    SetSizer(main_sizer);

    Bind(wxEVT_BUTTON, &SettingsDialog::on_ok, this, wxID_OK);
}

// ---------------------------------------------------------------------------
// LLM tab
// ---------------------------------------------------------------------------

wxPanel* SettingsDialog::build_llm_tab()
{
    auto* panel = new wxPanel(this);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* grid  = new wxFlexGridSizer(2, wxSize(8, 4));
    grid->AddGrowableCol(1, 1);

    grid->Add(new wxStaticText(panel, wxID_ANY, "Endpoint URL:"),
              0, wxALIGN_CENTER_VERTICAL);
    endpoint_ctrl_ = new wxTextCtrl(panel, wxID_ANY,
        wxString::FromUTF8(config_.llm_endpoint));
    grid->Add(endpoint_ctrl_, 1, wxEXPAND);

    grid->Add(new wxStaticText(panel, wxID_ANY, "Model:"),
              0, wxALIGN_CENTER_VERTICAL);
    model_ctrl_ = new wxTextCtrl(panel, wxID_ANY,
        wxString::FromUTF8(config_.llm_model));
    grid->Add(model_ctrl_, 1, wxEXPAND);

    grid->Add(new wxStaticText(panel, wxID_ANY, "Temperature:"),
              0, wxALIGN_CENTER_VERTICAL);
    temperature_ctrl_ = new wxSpinCtrlDouble(panel, wxID_ANY);
    temperature_ctrl_->SetRange(0.0, 2.0);
    temperature_ctrl_->SetIncrement(0.1);
    temperature_ctrl_->SetDigits(2);
    temperature_ctrl_->SetValue(config_.llm_temperature);
    grid->Add(temperature_ctrl_, 0);

    grid->Add(new wxStaticText(panel, wxID_ANY, "Context limit:"),
              0, wxALIGN_CENTER_VERTICAL);
    context_ctrl_ = new wxSpinCtrl(panel, wxID_ANY);
    context_ctrl_->SetRange(0, 1048576);
    context_ctrl_->SetValue(config_.llm_context_limit);
    grid->Add(context_ctrl_, 0);

    grid->Add(new wxStaticText(panel, wxID_ANY, "Max tokens (per response):"),
              0, wxALIGN_CENTER_VERTICAL);
    max_tokens_ctrl_ = new wxSpinCtrl(panel, wxID_ANY);
    max_tokens_ctrl_->SetRange(256, 1048576);
    max_tokens_ctrl_->SetValue(config_.llm_max_tokens > 0 ? config_.llm_max_tokens : 8192);
    grid->Add(max_tokens_ctrl_, 0);

    auto hint_font = panel->GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);

    auto* ctx_hint = new wxStaticText(panel, wxID_ANY,
        "(0 = auto-detect context limit from server)");
    ctx_hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    ctx_hint->SetFont(hint_font);

    auto* mt_hint = new wxStaticText(panel, wxID_ANY,
        "Max tokens caps a single response. Bump if multi-file edits get cut off.");
    mt_hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    mt_hint->SetFont(hint_font);

    outer->Add(grid,     0, wxEXPAND | wxALL, 8);
    outer->Add(ctx_hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    outer->Add(mt_hint,  0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    panel->SetSizer(outer);
    return panel;
}

// ---------------------------------------------------------------------------
// Index tab
// ---------------------------------------------------------------------------

wxPanel* SettingsDialog::build_index_tab()
{
    auto* panel = new wxPanel(this);
    auto* outer = new wxBoxSizer(wxVERTICAL);

    outer->Add(new wxStaticText(panel, wxID_ANY, "Exclude patterns (one per line):"),
               0, wxLEFT | wxRIGHT | wxTOP, 8);

    std::string patterns_text;
    for (size_t i = 0; i < config_.exclude_patterns.size(); ++i) {
        if (i > 0) patterns_text += '\n';
        patterns_text += config_.exclude_patterns[i];
    }
    exclude_ctrl_ = new wxTextCtrl(panel, wxID_ANY,
        wxString::FromUTF8(patterns_text),
        wxDefaultPosition, wxSize(-1, 100),
        wxTE_MULTILINE | wxHSCROLL);
    outer->Add(exclude_ctrl_, 1, wxEXPAND | wxALL, 8);

    semantic_enabled_ctrl_ = new wxCheckBox(panel, wxID_ANY, "Enable semantic search");
    semantic_enabled_ctrl_->SetValue(config_.semantic_search_enabled);
    outer->Add(semantic_enabled_ctrl_, 0, wxLEFT | wxRIGHT, 8);

    auto* sem_grid = new wxFlexGridSizer(2, wxSize(8, 4));
    sem_grid->AddGrowableCol(1, 1);
    sem_grid->Add(new wxStaticText(panel, wxID_ANY, "Semantic search model:"),
                  0, wxALIGN_CENTER_VERTICAL);
    semantic_model_ctrl_ = new wxTextCtrl(panel, wxID_ANY,
        wxString::FromUTF8(config_.embedding_model));
    sem_grid->Add(semantic_model_ctrl_, 1, wxEXPAND);

    sem_grid->Add(new wxStaticText(panel, wxID_ANY, "Reranker model:"),
                  0, wxALIGN_CENTER_VERTICAL);
    reranker_model_ctrl_ = new wxTextCtrl(panel, wxID_ANY,
        wxString::FromUTF8(config_.reranker_model));
    sem_grid->Add(reranker_model_ctrl_, 1, wxEXPAND);

    sem_grid->Add(new wxStaticText(panel, wxID_ANY, "Reranker candidate pool (top-K):"),
                  0, wxALIGN_CENTER_VERTICAL);
    reranker_top_k_ctrl_ = new wxSpinCtrl(panel, wxID_ANY);
    reranker_top_k_ctrl_->SetRange(1, 200);
    reranker_top_k_ctrl_->SetValue(config_.reranker_top_k);
    sem_grid->Add(reranker_top_k_ctrl_, 0);

    outer->Add(sem_grid, 0, wxEXPAND | wxALL, 8);

    reranker_enabled_ctrl_ = new wxCheckBox(panel, wxID_ANY,
        "Enable cross-encoder reranker (slower, more accurate top-N)");
    reranker_enabled_ctrl_->SetValue(config_.reranker_enabled);
    outer->Add(reranker_enabled_ctrl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    panel->SetSizer(outer);
    return panel;
}

// ---------------------------------------------------------------------------
// Tool Approvals tab
// ---------------------------------------------------------------------------

wxPanel* SettingsDialog::build_approvals_tab()
{
    auto* panel = new wxPanel(this);
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* scroll = new wxScrolledWindow(panel, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxVSCROLL | wxBORDER_SIMPLE);
    scroll->SetScrollRate(0, 12);

    auto* scroll_sizer = new wxFlexGridSizer(2, wxSize(8, 4));
    scroll_sizer->AddGrowableCol(0, 1);

    std::vector<ITool*> sorted_tools = tools_.all();
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

        ToolApprovalPolicy effective = tool->approval_policy();
        auto it = config_.tool_approval_policies.find(tname);
        if (it != config_.tool_approval_policies.end())
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

    outer->Add(scroll, 1, wxEXPAND | wxALL, 8);
    panel->SetSizer(outer);
    return panel;
}

// ---------------------------------------------------------------------------
// MCP Servers tab
// ---------------------------------------------------------------------------

wxPanel* SettingsDialog::build_mcp_tab()
{
    auto* panel = new wxPanel(this);
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = panel->GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);

    if (!mcp_) {
        // No manager available (e.g. workspace open path that pre-dates S4.G).
        // Just point the user at the schema doc and bail.
        auto* hint = new wxStaticText(panel, wxID_ANY,
            "MCP support is initialised by LocusSession at startup. "
            "Create .locus/mcp.json (or %APPDATA%/Locus/mcp.json) and "
            "restart Locus to enable Model Context Protocol servers.\n\n"
            "Schema: architecture/mcp.md");
        outer->Add(hint, 0, wxALL, 12);
        panel->SetSizer(outer);
        return panel;
    }

    auto* hint = new wxStaticText(panel, wxID_ANY,
        "Servers loaded from .locus/mcp.json and %APPDATA%/Locus/mcp.json. "
        "Edit the file, then click Restart to re-spawn a server in place.");
    hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    hint->SetFont(hint_font);
    outer->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    mcp_list_ = new wxListCtrl(panel, k_mcp_list_id,
        wxDefaultPosition, wxSize(-1, 160),
        wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    mcp_list_->AppendColumn("Server",  wxLIST_FORMAT_LEFT, 110);
    mcp_list_->AppendColumn("Status",  wxLIST_FORMAT_LEFT, 90);
    mcp_list_->AppendColumn("Tools",   wxLIST_FORMAT_LEFT, 60);
    mcp_list_->AppendColumn("Command", wxLIST_FORMAT_LEFT, 280);
    outer->Add(mcp_list_, 1, wxEXPAND | wxALL, 8);

    auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
    mcp_restart_btn_ = new wxButton(panel, k_mcp_restart_id, "Restart");
    mcp_open_btn_    = new wxButton(panel, k_mcp_open_id,    "Open mcp.json");
    mcp_trust_       = new wxCheckBox(panel, wxID_ANY,
        "Trust selected server (auto-approve all mcp:<server>:* tools)");
    btn_row->Add(mcp_restart_btn_, 0, wxRIGHT, 6);
    btn_row->Add(mcp_open_btn_,    0, wxRIGHT, 18);
    btn_row->Add(mcp_trust_, 1, wxALIGN_CENTER_VERTICAL);
    outer->Add(btn_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    mcp_detail_ = new wxStaticText(panel, wxID_ANY, "",
        wxDefaultPosition, wxSize(-1, 80), wxST_NO_AUTORESIZE);
    mcp_detail_->SetFont(hint_font);
    outer->Add(mcp_detail_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    panel->SetSizer(outer);

    // Seed initial trust state so OK can diff against it.
    auto snap = mcp_->status_snapshot();
    mcp_initial_trust_.reserve(snap.size());
    mcp_current_trust_.reserve(snap.size());
    for (const auto& s : snap) {
        auto it = config_.tool_approval_policies.find(trust_key(s.name));
        bool trusted = (it != config_.tool_approval_policies.end() &&
                        it->second == ToolApprovalPolicy::auto_approve);
        mcp_initial_trust_.emplace_back(s.name, trusted);
        mcp_current_trust_.push_back(trusted);
    }

    // Button + select bindings.
    Bind(wxEVT_BUTTON, &SettingsDialog::on_mcp_restart, this, k_mcp_restart_id);
    Bind(wxEVT_BUTTON, &SettingsDialog::on_mcp_open_json, this, k_mcp_open_id);
    Bind(wxEVT_LIST_ITEM_SELECTED, &SettingsDialog::on_mcp_select, this, k_mcp_list_id);

    mcp_trust_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
        if (mcp_selected_ < 0) return;
        mcp_current_trust_[mcp_selected_] = mcp_trust_->GetValue();
        e.Skip();
    });

    refresh_mcp_list();
    if (mcp_list_->GetItemCount() > 0) {
        mcp_list_->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    } else {
        mcp_restart_btn_->Disable();
        mcp_trust_->Disable();
        mcp_detail_->SetLabel("No MCP servers configured.");
    }
    return panel;
}

void SettingsDialog::refresh_mcp_list()
{
    if (!mcp_list_ || !mcp_) return;

    long preserve_sel = mcp_selected_;
    mcp_list_->DeleteAllItems();

    auto snap = mcp_->status_snapshot();
    for (size_t i = 0; i < snap.size(); ++i) {
        const auto& s = snap[i];
        long row = mcp_list_->InsertItem(static_cast<long>(i),
            wxString::FromUTF8(s.name));
        mcp_list_->SetItem(row, 1, mcp_status_label(s.status));
        mcp_list_->SetItem(row, 2,
            wxString::Format("%zu", s.tool_names.size()));
        mcp_list_->SetItem(row, 3, wxString::FromUTF8(s.command));
    }
    if (preserve_sel >= 0 && preserve_sel < mcp_list_->GetItemCount()) {
        mcp_list_->SetItemState(preserve_sel, wxLIST_STATE_SELECTED,
            wxLIST_STATE_SELECTED);
    }
}

void SettingsDialog::on_mcp_select(wxListEvent& evt)
{
    mcp_selected_ = static_cast<int>(evt.GetIndex());
    auto snap = mcp_->status_snapshot();
    if (mcp_selected_ < 0 || mcp_selected_ >= static_cast<int>(snap.size())) {
        mcp_detail_->SetLabel("");
        return;
    }
    const auto& s = snap[mcp_selected_];

    std::string detail = "Status: " + std::string(mcp_status_label(s.status).utf8_string());
    if (s.has_exit_code)
        detail += " (exit code " + std::to_string(s.exit_code) + ")";
    if (!s.tool_names.empty()) {
        detail += "\nTools: ";
        for (size_t i = 0; i < s.tool_names.size(); ++i) {
            if (i > 0) detail += ", ";
            detail += s.tool_names[i];
        }
    }
    if (!s.last_error.empty()) {
        detail += "\nLast error: " + s.last_error;
    }
    mcp_detail_->SetLabel(wxString::FromUTF8(detail));

    if (mcp_trust_)
        mcp_trust_->SetValue(mcp_current_trust_[mcp_selected_]);
}

void SettingsDialog::on_mcp_restart(wxCommandEvent&)
{
    if (!mcp_ || mcp_selected_ < 0) return;
    auto snap = mcp_->status_snapshot();
    if (mcp_selected_ >= static_cast<int>(snap.size())) return;

    const std::string name = snap[mcp_selected_].name;
    bool ok = mcp_->restart(name);
    refresh_mcp_list();
    // Re-fire the selection so the detail box refreshes with new state.
    if (mcp_selected_ < mcp_list_->GetItemCount()) {
        wxListEvent evt(wxEVT_LIST_ITEM_SELECTED, k_mcp_list_id);
        evt.m_itemIndex = mcp_selected_;
        on_mcp_select(evt);
    }
    if (!ok) {
        wxMessageBox(wxString::Format(
            "Failed to restart MCP server '%s'. See the activity log "
            "for details.", wxString::FromUTF8(name)),
            "MCP", wxOK | wxICON_WARNING, this);
    }
}

void SettingsDialog::on_mcp_open_json(wxCommandEvent&)
{
    if (!mcp_) return;
    auto path = McpConfigLoader::workspace_config_path(mcp_->workspace_root());

    // Create an empty stub so the user has something to edit if the file
    // doesn't exist yet. Mirrors what `git init` does for .gitignore-style
    // touch-and-open flows.
    if (!std::filesystem::exists(path)) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path);
        if (f.is_open()) {
            f << "{\n"
                 "  \"mcpServers\": {\n"
                 "    // \"example\": { \"command\": \"npx\", \"args\": [\"-y\", \"@modelcontextprotocol/server-filesystem\", \".\"] }\n"
                 "  }\n"
                 "}\n";
        }
    }

    if (!wxLaunchDefaultApplication(wxString::FromUTF8(path.string()))) {
        wxMessageBox(wxString::Format("Could not open %s in the default editor.\n"
                                      "Path is on the clipboard if you want to open it manually.",
                                      wxString::FromUTF8(path.string())),
                     "MCP", wxOK | wxICON_INFORMATION, this);
    }
}

// ---------------------------------------------------------------------------
// OK handler
// ---------------------------------------------------------------------------

void SettingsDialog::on_ok(wxCommandEvent& evt)
{
    std::string new_endpoint = endpoint_ctrl_->GetValue().ToStdString();
    std::string new_model    = model_ctrl_->GetValue().ToStdString();
    double      new_temp     = temperature_ctrl_->GetValue();
    int         new_context  = context_ctrl_->GetValue();
    int         new_max_tok  = max_tokens_ctrl_->GetValue();

    std::vector<std::string> new_patterns;
    {
        std::string text = exclude_ctrl_->GetValue().ToStdString();
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            auto start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos) continue;
            auto end = line.find_last_not_of(" \t\r");
            new_patterns.push_back(line.substr(start, end - start + 1));
        }
    }

    if (new_endpoint != config_.llm_endpoint ||
        new_model    != config_.llm_model ||
        new_temp     != config_.llm_temperature ||
        new_context  != config_.llm_context_limit ||
        new_max_tok  != config_.llm_max_tokens) {
        llm_changed_ = true;
    }

    bool new_semantic           = semantic_enabled_ctrl_->GetValue();
    std::string new_sem_model   = semantic_model_ctrl_->GetValue().ToStdString();
    bool new_reranker_enabled   = reranker_enabled_ctrl_->GetValue();
    std::string new_reranker_md = reranker_model_ctrl_->GetValue().ToStdString();
    int  new_reranker_top_k     = reranker_top_k_ctrl_->GetValue();

    if (new_patterns != config_.exclude_patterns)
        index_changed_ = true;
    if (new_semantic != config_.semantic_search_enabled ||
        new_sem_model != config_.embedding_model ||
        new_reranker_enabled != config_.reranker_enabled ||
        new_reranker_md != config_.reranker_model ||
        new_reranker_top_k != config_.reranker_top_k)
        semantic_changed_ = true;

    // Tool approval map: rebuild from the per-tool choices, then layer the
    // MCP trust toggles on top so a "trust mcp:foo:*" entry survives even
    // though no per-tool row exists for it.
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

    // Preserve any existing prefix overrides the user has typed into
    // config.json by hand -- the dialog only manages the ones it knows
    // about (the current MCP server set). Everything that wasn't in
    // mcp_initial_trust_ stays untouched.
    for (const auto& [key, val] : config_.tool_approval_policies) {
        if (key.size() < 2 || key.compare(key.size() - 2, 2, ":*") != 0) continue;
        bool managed = false;
        for (const auto& [name, _t] : mcp_initial_trust_)
            if (key == trust_key(name)) { managed = true; break; }
        if (!managed) new_approvals[key] = val;
    }

    // Apply the current MCP trust state.
    for (size_t i = 0; i < mcp_initial_trust_.size(); ++i) {
        const std::string key = trust_key(mcp_initial_trust_[i].first);
        if (mcp_current_trust_[i]) {
            new_approvals[key] = ToolApprovalPolicy::auto_approve;
        } // false means no entry, which is the implicit "ask via the McpTool default"
    }

    bool approvals_changed = (new_approvals != config_.tool_approval_policies);

    changed_ = llm_changed_ || index_changed_ || semantic_changed_ || approvals_changed;

    if (changed_) {
        config_.llm_endpoint      = new_endpoint;
        config_.llm_model         = new_model;
        config_.llm_temperature   = new_temp;
        config_.llm_context_limit = new_context;
        config_.llm_max_tokens    = new_max_tok;
        config_.exclude_patterns  = new_patterns;
        config_.semantic_search_enabled = new_semantic;
        config_.embedding_model   = new_sem_model;
        config_.reranker_enabled  = new_reranker_enabled;
        config_.reranker_model    = new_reranker_md;
        config_.reranker_top_k    = new_reranker_top_k;
        config_.tool_approval_policies = std::move(new_approvals);

        spdlog::info("Settings changed (llm={}, index={}, semantic={}, approvals={})",
                     llm_changed_, index_changed_, semantic_changed_, approvals_changed);
    }

    evt.Skip();
}

} // namespace locus
