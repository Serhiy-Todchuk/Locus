#include "settings_dialog.h"

#include "locus_accessible.h"
#include "ui_names.h"
#include "../../core/global_config.h"
#include "../../core/global_paths.h"
#include "../../llm/model_presets.h"
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

// S4.V Task 4 -- tools whose Auto-Approve toggle requires a friction confirm.
// Membership is intentionally narrow: read-only tools and search are fine to
// auto-approve silently; only persistent side effects warrant the speed bump.
// MCP tools follow their own per-server "trust" toggle in the MCP tab.
bool is_mutating_tool(const std::string& name)
{
    return name == "edit_file"
        || name == "write_file"
        || name == "delete_file"
        || name == "run_command"
        || name == "run_command_bg";
}

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
    SetName(ui_names::kSettingsDialog);
    gui::apply_locus_accessible_name(this);

    auto* notebook = new wxNotebook(this, wxID_ANY);
    notebook->SetName(ui_names::kSettingsNotebook);
    gui::apply_locus_accessible_name(notebook);

    auto* tab_llm          = build_llm_tab(notebook);
    auto* tab_index        = build_index_tab(notebook);
    auto* tab_capabilities = build_capabilities_tab(notebook);
    auto* tab_approvals    = build_approvals_tab(notebook);
    auto* tab_mcp          = build_mcp_tab(notebook);

    tab_llm->SetName(ui_names::kSettingsTabLlm);
    tab_index->SetName(ui_names::kSettingsTabIndex);
    tab_capabilities->SetName(ui_names::kSettingsTabCapabilities);
    tab_approvals->SetName(ui_names::kSettingsTabApprovals);
    tab_mcp->SetName(ui_names::kSettingsTabMcp);
    gui::apply_locus_accessible_name(tab_llm);
    gui::apply_locus_accessible_name(tab_index);
    gui::apply_locus_accessible_name(tab_capabilities);
    gui::apply_locus_accessible_name(tab_approvals);
    gui::apply_locus_accessible_name(tab_mcp);

    notebook->AddPage(tab_llm,          "LLM");
    notebook->AddPage(tab_index,        "Index");
    notebook->AddPage(tab_capabilities, "Capabilities");
    notebook->AddPage(tab_approvals,    "Tool Approvals");
    notebook->AddPage(tab_mcp,          "MCP Servers");

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(notebook, 1, wxEXPAND | wxALL, 8);

    // S5.M -- bottom button row: [Save as global defaults]  [spacer]  [OK] [Cancel].
    // The global-save button captures the current dialog state as the template
    // for any new workspace opened from this point on; it does NOT touch the
    // current workspace's config (still gated by OK).
    auto* button_row = new wxBoxSizer(wxHORIZONTAL);
    auto* save_global_btn = new wxButton(this, wxID_ANY, "Save as global defaults");
    save_global_btn->SetToolTip(
        "Snapshot the current Settings values into ~/.locus/config.json. "
        "Future workspaces will use them as their starting template. "
        "Existing workspaces are not affected.");
    save_global_btn->Bind(wxEVT_BUTTON,
                          &SettingsDialog::on_save_as_global_defaults, this);
    button_row->Add(save_global_btn, 0, wxALIGN_CENTER_VERTICAL);
    button_row->AddStretchSpacer(1);
    button_row->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
                    wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(button_row, 0, wxEXPAND | wxALL, 8);

    SetSizer(main_sizer);

    Bind(wxEVT_BUTTON, &SettingsDialog::on_ok, this, wxID_OK);
}

// ---------------------------------------------------------------------------
// LLM tab
// ---------------------------------------------------------------------------

wxPanel* SettingsDialog::build_llm_tab(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = panel->GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);

    // S4.V Task 6 -- preset row at the top. Picking a preset and clicking
    // "Load preset" overwrites endpoint / temperature / max_tokens / tool
    // format. Model name is deliberately NOT touched (every install has
    // different specific files on disk).
    {
        auto* preset_row = new wxBoxSizer(wxHORIZONTAL);
        preset_row->Add(new wxStaticText(panel, wxID_ANY, "Preset:"),
                        0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        preset_ctrl_ = new wxChoice(panel, wxID_ANY);
        preset_ctrl_->Append("Custom");
        for (const auto& p : builtin_presets())
            preset_ctrl_->Append(wxString::FromUTF8(p.name));
        preset_ctrl_->SetSelection(0);
        preset_ctrl_->Bind(wxEVT_CHOICE, &SettingsDialog::on_preset_choice, this);
        preset_row->Add(preset_ctrl_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        auto* apply_btn = new wxButton(panel, wxID_ANY, "Load preset");
        apply_btn->Bind(wxEVT_BUTTON, &SettingsDialog::on_preset_apply, this);
        preset_row->Add(apply_btn, 0, wxALIGN_CENTER_VERTICAL);

        outer->Add(preset_row, 0, wxEXPAND | wxALL, 8);

        preset_hint_ = new wxStaticText(panel, wxID_ANY,
            "Static known-good defaults per backend + model family. "
            "Overwrites endpoint, temperature, max tokens, and tool format. "
            "Type the actual model id below.");
        preset_hint_->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        preset_hint_->SetFont(hint_font);
        preset_hint_->Wrap(420);
        outer->Add(preset_hint_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    }

    auto* grid  = new wxFlexGridSizer(2, wxSize(8, 4));
    grid->AddGrowableCol(1, 1);

    // Tooltip strings (hover help). User-facing descriptions; ASCII only.
    // Keep these short enough to fit a tooltip balloon but long enough that
    // the user understands the trade-off without leaving the dialog.
    static constexpr const char* kTipEndpoint =
        "Base URL of the local OpenAI-compatible LLM server "
        "(default LM Studio at http://127.0.0.1:1234). The "
        "/v1/chat/completions path is appended automatically.";
    static constexpr const char* kTipModel =
        "Optional model id to request. Leave empty to use the model the "
        "server has loaded. Set this when the server hosts multiple models "
        "or supports model switching.";
    static constexpr const char* kTipTemperature =
        "Sampling temperature (0 - 2). Higher values produce less "
        "deterministic, more creative output; lower values produce more "
        "focused, repeatable output. It is not recommended to modify both "
        "temperature and top_p in the same call.";
    static constexpr const char* kTipContextLimit =
        "Total context window the agent fills before triggering compaction. "
        "0 = auto-detect from the server (the model's trained ceiling). Set "
        "lower than the model max if you want compaction to kick in earlier "
        "and keep prefill snappy.";
    static constexpr const char* kTipMaxTokens =
        "Maximum number of tokens to generate in any given call. The model "
        "is not aware of this value; generation simply stops when the cap "
        "is hit. Bump this if multi-file edits get cut off mid-response.";
    static constexpr const char* kTipToolFormat =
        "Wire format the model emits tool calls in. Auto runs the OpenAI "
        "JSON + Qwen XML + Claude XML decoders in parallel and is the right "
        "default for unknown models. Pin to a specific format only when you "
        "know the model. None skips the tools array entirely (for "
        "non-tool-trained base models).";
    static constexpr const char* kTipTopP =
        "Nucleus sampling (0.01 - 1). Samples from the smallest set of "
        "tokens whose cumulative probability is >= top_p. For example, "
        "top_p = 0.2 keeps only the most likely tokens summing to 20% mass. "
        "0 = don't send (server default). Avoid modifying both temperature "
        "and top_p in the same call.";
    static constexpr const char* kTipTopK =
        "llama.cpp / LM Studio extension. Limits sampling to the K most "
        "likely tokens at each step. Lower values are more deterministic; "
        "typical range 20 - 100. 0 = don't send (server default). Pure-"
        "OpenAI servers ignore this field.";
    static constexpr const char* kTipMinP =
        "llama.cpp extension. Drops tokens whose probability is less than "
        "min_p multiplied by the top token's probability. Acts as a noise "
        "floor; typical range 0.05 - 0.1. 0 = don't send (server default). "
        "Pure-OpenAI servers ignore this field.";
    static constexpr const char* kTipRepeatPenalty =
        "llama.cpp extension. Multiplicative penalty on tokens that have "
        "appeared in the recent context. >1.0 discourages repetition, "
        "<1.0 encourages it, 1.0 = neutral. 0 = don't send (server "
        "default). Distinct from the OpenAI frequency / presence penalties "
        "below -- the three knobs compose.";
    static constexpr const char* kTipFrequencyPenalty =
        "OpenAI penalty (-2 to 2). How much to penalize new tokens based "
        "on their existing frequency in the text so far, decreasing the "
        "model's likelihood to repeat the same line verbatim. 0 = don't "
        "send (server default, also OpenAI's neutral value).";
    static constexpr const char* kTipPresencePenalty =
        "OpenAI penalty (-2 to 2). Positive values penalize tokens based "
        "on whether they appear in the text at all, increasing the "
        "model's likelihood to talk about new topics. 0 = don't send "
        "(server default).";

    auto* lbl_endpoint = new wxStaticText(panel, wxID_ANY, "Endpoint URL:");
    lbl_endpoint->SetToolTip(kTipEndpoint);
    grid->Add(lbl_endpoint, 0, wxALIGN_CENTER_VERTICAL);
    endpoint_ctrl_ = new wxTextCtrl(panel, wxID_ANY,
        wxString::FromUTF8(config_.llm_endpoint));
    endpoint_ctrl_->SetToolTip(kTipEndpoint);
    grid->Add(endpoint_ctrl_, 1, wxEXPAND);

    auto* lbl_model = new wxStaticText(panel, wxID_ANY, "Model:");
    lbl_model->SetToolTip(kTipModel);
    grid->Add(lbl_model, 0, wxALIGN_CENTER_VERTICAL);
    model_ctrl_ = new wxTextCtrl(panel, wxID_ANY,
        wxString::FromUTF8(config_.llm_model));
    model_ctrl_->SetToolTip(kTipModel);
    grid->Add(model_ctrl_, 1, wxEXPAND);

    auto* lbl_temp = new wxStaticText(panel, wxID_ANY, "Temperature:");
    lbl_temp->SetToolTip(kTipTemperature);
    grid->Add(lbl_temp, 0, wxALIGN_CENTER_VERTICAL);
    temperature_ctrl_ = new wxSpinCtrlDouble(panel, wxID_ANY);
    temperature_ctrl_->SetRange(0.0, 2.0);
    temperature_ctrl_->SetIncrement(0.1);
    temperature_ctrl_->SetDigits(2);
    temperature_ctrl_->SetValue(config_.llm_temperature);
    temperature_ctrl_->SetToolTip(kTipTemperature);
    grid->Add(temperature_ctrl_, 0);

    auto* lbl_ctx = new wxStaticText(panel, wxID_ANY, "Context limit:");
    lbl_ctx->SetToolTip(kTipContextLimit);
    grid->Add(lbl_ctx, 0, wxALIGN_CENTER_VERTICAL);
    context_ctrl_ = new wxSpinCtrl(panel, wxID_ANY);
    context_ctrl_->SetRange(0, 1048576);
    context_ctrl_->SetValue(config_.llm_context_limit);
    context_ctrl_->SetToolTip(kTipContextLimit);
    grid->Add(context_ctrl_, 0);

    auto* lbl_max = new wxStaticText(panel, wxID_ANY, "Max tokens (per response):");
    lbl_max->SetToolTip(kTipMaxTokens);
    grid->Add(lbl_max, 0, wxALIGN_CENTER_VERTICAL);
    max_tokens_ctrl_ = new wxSpinCtrl(panel, wxID_ANY);
    max_tokens_ctrl_->SetRange(256, 1048576);
    max_tokens_ctrl_->SetValue(config_.llm_max_tokens > 0 ? config_.llm_max_tokens : 8192);
    max_tokens_ctrl_->SetToolTip(kTipMaxTokens);
    grid->Add(max_tokens_ctrl_, 0);

    // Tool format -- ToolFormat enum exposed verbatim. Auto is the default and
    // handles unknown models; Qwen / Claude pin XML extraction; None skips
    // the tools array entirely for non-tool-trained base models.
    auto* lbl_tf = new wxStaticText(panel, wxID_ANY, "Tool-call format:");
    lbl_tf->SetToolTip(kTipToolFormat);
    grid->Add(lbl_tf, 0, wxALIGN_CENTER_VERTICAL);
    tool_format_ctrl_ = new wxChoice(panel, wxID_ANY);
    tool_format_ctrl_->Append("Auto");
    tool_format_ctrl_->Append("OpenAI");
    tool_format_ctrl_->Append("Qwen");
    tool_format_ctrl_->Append("Claude");
    tool_format_ctrl_->Append("None");
    {
        ToolFormat tf = tool_format_from_string(config_.llm_tool_format);
        int sel = 0;
        switch (tf) {
            case ToolFormat::Auto:   sel = 0; break;
            case ToolFormat::OpenAi: sel = 1; break;
            case ToolFormat::Qwen:   sel = 2; break;
            case ToolFormat::Claude: sel = 3; break;
            case ToolFormat::None:   sel = 4; break;
        }
        tool_format_ctrl_->SetSelection(sel);
    }
    tool_format_ctrl_->SetToolTip(kTipToolFormat);
    grid->Add(tool_format_ctrl_, 0);

    // S4.V Task 7 -- sampler overrides. 0 in any spinner means "do not send
    // this field" so the server's per-model default applies. The ranges are
    // chosen to be generous (the LLM server is the real validator); the
    // wxSpinCtrlDouble step is 0.05 because sampler dials are usually tuned
    // in small increments rather than free-form decimals.
    auto* lbl_top_p = new wxStaticText(panel, wxID_ANY, "top_p:");
    lbl_top_p->SetToolTip(kTipTopP);
    grid->Add(lbl_top_p, 0, wxALIGN_CENTER_VERTICAL);
    top_p_ctrl_ = new wxSpinCtrlDouble(panel, wxID_ANY);
    top_p_ctrl_->SetRange(0.0, 1.0);
    top_p_ctrl_->SetIncrement(0.05);
    top_p_ctrl_->SetDigits(2);
    top_p_ctrl_->SetValue(config_.llm_top_p);
    top_p_ctrl_->SetToolTip(kTipTopP);
    grid->Add(top_p_ctrl_, 0);

    auto* lbl_top_k = new wxStaticText(panel, wxID_ANY, "top_k:");
    lbl_top_k->SetToolTip(kTipTopK);
    grid->Add(lbl_top_k, 0, wxALIGN_CENTER_VERTICAL);
    top_k_ctrl_ = new wxSpinCtrl(panel, wxID_ANY);
    top_k_ctrl_->SetRange(0, 10000);
    top_k_ctrl_->SetValue(config_.llm_top_k);
    top_k_ctrl_->SetToolTip(kTipTopK);
    grid->Add(top_k_ctrl_, 0);

    auto* lbl_min_p = new wxStaticText(panel, wxID_ANY, "min_p:");
    lbl_min_p->SetToolTip(kTipMinP);
    grid->Add(lbl_min_p, 0, wxALIGN_CENTER_VERTICAL);
    min_p_ctrl_ = new wxSpinCtrlDouble(panel, wxID_ANY);
    min_p_ctrl_->SetRange(0.0, 1.0);
    min_p_ctrl_->SetIncrement(0.01);
    min_p_ctrl_->SetDigits(2);
    min_p_ctrl_->SetValue(config_.llm_min_p);
    min_p_ctrl_->SetToolTip(kTipMinP);
    grid->Add(min_p_ctrl_, 0);

    auto* lbl_rp = new wxStaticText(panel, wxID_ANY, "repeat_penalty:");
    lbl_rp->SetToolTip(kTipRepeatPenalty);
    grid->Add(lbl_rp, 0, wxALIGN_CENTER_VERTICAL);
    repeat_penalty_ctrl_ = new wxSpinCtrlDouble(panel, wxID_ANY);
    repeat_penalty_ctrl_->SetRange(0.0, 2.0);
    repeat_penalty_ctrl_->SetIncrement(0.05);
    repeat_penalty_ctrl_->SetDigits(2);
    repeat_penalty_ctrl_->SetValue(config_.llm_repeat_penalty);
    repeat_penalty_ctrl_->SetToolTip(kTipRepeatPenalty);
    grid->Add(repeat_penalty_ctrl_, 0);

    // OpenAI-protocol penalties. Range [-2, 2]; 0 = don't send.
    auto* lbl_fp = new wxStaticText(panel, wxID_ANY, "frequency_penalty:");
    lbl_fp->SetToolTip(kTipFrequencyPenalty);
    grid->Add(lbl_fp, 0, wxALIGN_CENTER_VERTICAL);
    frequency_penalty_ctrl_ = new wxSpinCtrlDouble(panel, wxID_ANY);
    frequency_penalty_ctrl_->SetRange(-2.0, 2.0);
    frequency_penalty_ctrl_->SetIncrement(0.05);
    frequency_penalty_ctrl_->SetDigits(2);
    frequency_penalty_ctrl_->SetValue(config_.llm_frequency_penalty);
    frequency_penalty_ctrl_->SetToolTip(kTipFrequencyPenalty);
    grid->Add(frequency_penalty_ctrl_, 0);

    auto* lbl_pp = new wxStaticText(panel, wxID_ANY, "presence_penalty:");
    lbl_pp->SetToolTip(kTipPresencePenalty);
    grid->Add(lbl_pp, 0, wxALIGN_CENTER_VERTICAL);
    presence_penalty_ctrl_ = new wxSpinCtrlDouble(panel, wxID_ANY);
    presence_penalty_ctrl_->SetRange(-2.0, 2.0);
    presence_penalty_ctrl_->SetIncrement(0.05);
    presence_penalty_ctrl_->SetDigits(2);
    presence_penalty_ctrl_->SetValue(config_.llm_presence_penalty);
    presence_penalty_ctrl_->SetToolTip(kTipPresencePenalty);
    grid->Add(presence_penalty_ctrl_, 0);

    auto* ctx_hint = new wxStaticText(panel, wxID_ANY,
        "(0 = auto-detect context limit from server)");
    ctx_hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    ctx_hint->SetFont(hint_font);

    auto* mt_hint = new wxStaticText(panel, wxID_ANY,
        "Max tokens caps a single response. Bump if multi-file edits get cut off.");
    mt_hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    mt_hint->SetFont(hint_font);

    auto* sampler_hint = new wxStaticText(panel, wxID_ANY,
        "Sampler overrides: 0 = use server's per-model default. Set positive "
        "values only if you know what you're doing -- the server's defaults "
        "are usually right for the loaded model.");
    sampler_hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    sampler_hint->SetFont(hint_font);
    sampler_hint->Wrap(420);

    outer->Add(grid,         0, wxEXPAND | wxALL, 8);
    outer->Add(ctx_hint,     0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    outer->Add(mt_hint,      0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    outer->Add(sampler_hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    panel->SetSizer(outer);
    return panel;
}

// S4.V Task 6 -- preset handlers.
//
// `on_preset_choice` updates the hint label so the user sees what the
// pick means before committing; `on_preset_apply` writes the preset's
// values into the relevant LLM-tab controls. "Custom" is the first entry
// and a no-op on apply (it represents whatever the user typed manually).

void SettingsDialog::on_preset_choice(wxCommandEvent&)
{
    if (!preset_ctrl_ || !preset_hint_) return;
    int sel = preset_ctrl_->GetSelection();
    if (sel <= 0) {
        preset_hint_->SetLabel("Static known-good defaults per backend + "
                               "model family. Overwrites endpoint, "
                               "temperature, max tokens, and tool format. "
                               "Type the actual model id below.");
    } else {
        const auto& p = builtin_presets()[static_cast<size_t>(sel - 1)];
        preset_hint_->SetLabel(wxString::FromUTF8(p.description));
    }
    preset_hint_->Wrap(420);
    preset_hint_->GetParent()->Layout();
}

void SettingsDialog::on_preset_apply(wxCommandEvent&)
{
    if (!preset_ctrl_) return;
    int sel = preset_ctrl_->GetSelection();
    if (sel <= 0) return;  // "Custom"

    const auto& p = builtin_presets()[static_cast<size_t>(sel - 1)];

    endpoint_ctrl_->ChangeValue(wxString::FromUTF8(p.base_url));
    temperature_ctrl_->SetValue(p.temperature);
    max_tokens_ctrl_->SetValue(p.max_tokens);

    int tf_sel = 0;
    switch (p.tool_format) {
        case ToolFormat::Auto:   tf_sel = 0; break;
        case ToolFormat::OpenAi: tf_sel = 1; break;
        case ToolFormat::Qwen:   tf_sel = 2; break;
        case ToolFormat::Claude: tf_sel = 3; break;
        case ToolFormat::None:   tf_sel = 4; break;
    }
    tool_format_ctrl_->SetSelection(tf_sel);

    // S4.V Task 7 -- write sampler defaults from the preset. Presets that
    // leave a sampler at 0 will visibly clear the corresponding spinner;
    // that matches the "load preset = adopt its recommendations" contract.
    if (top_p_ctrl_)          top_p_ctrl_->SetValue(p.top_p);
    if (top_k_ctrl_)          top_k_ctrl_->SetValue(p.top_k);
    if (min_p_ctrl_)          min_p_ctrl_->SetValue(p.min_p);
    if (repeat_penalty_ctrl_) repeat_penalty_ctrl_->SetValue(p.repeat_penalty);
}

// ---------------------------------------------------------------------------
// Index tab
// ---------------------------------------------------------------------------

wxPanel* SettingsDialog::build_index_tab(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
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

bool SettingsDialog::confirm_auto_approve_mutating(const std::string& tool_name)
{
    if (!is_mutating_tool(tool_name)) return true;

    wxString msg = wxString::Format(
        "Auto-approve every '%s' call in this workspace?\n\n"
        "Future calls will execute without prompting. Locus will still\n"
        "record each call in the activity log and snapshot affected files\n"
        "so /undo can roll back the most recent turn.\n\n"
        "Revoke any time in Settings -> Tool Approvals.",
        wxString::FromUTF8(tool_name));

    wxMessageDialog dlg(this, msg, "Auto-approve mutating tool?",
                        wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    dlg.SetYesNoLabels("Auto-approve", "Keep asking");
    return dlg.ShowModal() == wxID_YES;
}

wxPanel* SettingsDialog::build_approvals_tab(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
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
    approval_prev_sel_.reserve(sorted_tools.size());

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
        approval_prev_sel_.push_back(sel);

        // S4.V Task 4 -- intercept Auto-Approve picks on mutating tools and
        // show a one-time friction confirm. The lambda captures the index
        // into approval_choices_/tool_names_/approval_prev_sel_ so we can
        // snap back when the user declines.
        const size_t idx = approval_choices_.size() - 1;
        choice->Bind(wxEVT_CHOICE, [this, idx](wxCommandEvent&) {
            int new_sel = approval_choices_[idx]->GetSelection();
            if (new_sel == 1 /*Auto-Approve*/ &&
                approval_prev_sel_[idx] != 1 &&
                !confirm_auto_approve_mutating(tool_names_[idx]))
            {
                approval_choices_[idx]->SetSelection(approval_prev_sel_[idx]);
                return;
            }
            approval_prev_sel_[idx] = new_sel;
        });
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

wxPanel* SettingsDialog::build_mcp_tab(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
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
        "Servers loaded from .locus/mcp.json and %APPDATA%/Locus/mcp.json.\n"
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
    // touch-and-open flows. We accept jsonc on the read side (comments
    // ignored, see McpConfigLoader::parse_json), but the stub itself is
    // pure JSON so editors with strict highlighters don't flag it.
    if (!std::filesystem::exists(path)) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path);
        if (f.is_open()) {
            f << "{\n"
                 "  \"mcpServers\": {\n"
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

    // S4.V Task 6 -- ToolFormat dropdown.
    std::string new_tool_format = "auto";
    if (tool_format_ctrl_) {
        switch (tool_format_ctrl_->GetSelection()) {
            case 0: new_tool_format = "auto";   break;
            case 1: new_tool_format = "openai"; break;
            case 2: new_tool_format = "qwen";   break;
            case 3: new_tool_format = "claude"; break;
            case 4: new_tool_format = "none";   break;
        }
    }

    // S4.V Task 7 -- sampler overrides.
    double new_top_p          = top_p_ctrl_          ? top_p_ctrl_->GetValue()          : 0.0;
    int    new_top_k          = top_k_ctrl_          ? top_k_ctrl_->GetValue()          : 0;
    double new_min_p          = min_p_ctrl_          ? min_p_ctrl_->GetValue()          : 0.0;
    double new_repeat_penalty = repeat_penalty_ctrl_ ? repeat_penalty_ctrl_->GetValue() : 0.0;
    double new_frequency_penalty = frequency_penalty_ctrl_ ? frequency_penalty_ctrl_->GetValue() : 0.0;
    double new_presence_penalty  = presence_penalty_ctrl_  ? presence_penalty_ctrl_->GetValue()  : 0.0;

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
        new_max_tok  != config_.llm_max_tokens ||
        new_tool_format != config_.llm_tool_format ||
        new_top_p          != config_.llm_top_p ||
        new_top_k          != config_.llm_top_k ||
        new_min_p          != config_.llm_min_p ||
        new_repeat_penalty != config_.llm_repeat_penalty ||
        new_frequency_penalty != config_.llm_frequency_penalty ||
        new_presence_penalty  != config_.llm_presence_penalty) {
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

    // S5.A Capabilities tab. Reads off the five checkboxes built in
    // build_capabilities_tab. The semantic + memory checkboxes are the
    // canonical source for their legacy mirror flags -- assigning here
    // overwrites whatever the Index/Memory subsystems thought.
    WorkspaceConfig::Capabilities new_caps = config_.capabilities;
    if (cap_bg_)       new_caps.background_processes = cap_bg_->IsChecked();
    if (cap_semantic_) new_caps.semantic_search      = cap_semantic_->IsChecked();
    if (cap_code_)     new_caps.code_aware_search    = cap_code_->IsChecked();
    if (cap_memory_)   new_caps.memory_bank          = cap_memory_->IsChecked();
    if (cap_web_)      new_caps.web_retrieval        = cap_web_->IsChecked();

    if (new_caps.background_processes != config_.capabilities.background_processes ||
        new_caps.semantic_search      != config_.capabilities.semantic_search      ||
        new_caps.code_aware_search    != config_.capabilities.code_aware_search    ||
        new_caps.memory_bank          != config_.capabilities.memory_bank          ||
        new_caps.web_retrieval        != config_.capabilities.web_retrieval)
    {
        capabilities_changed_ = true;
    }

    // The capabilities semantic/memory bools are canonical -- propagate to
    // the Index-tab semantic checkbox + Workspace::memory_enabled mirror so
    // the next workspace open sees a consistent view.
    if (new_caps.semantic_search != new_semantic) {
        // Capabilities tab wins over the legacy Index tab when they disagree.
        new_semantic     = new_caps.semantic_search;
        semantic_changed_ = true;
    }

    changed_ = llm_changed_ || index_changed_ || semantic_changed_
            || approvals_changed || capabilities_changed_;

    if (changed_) {
        config_.llm_endpoint      = new_endpoint;
        config_.llm_model         = new_model;
        config_.llm_temperature   = new_temp;
        config_.llm_context_limit = new_context;
        config_.llm_max_tokens    = new_max_tok;
        config_.llm_tool_format   = new_tool_format;
        config_.llm_top_p          = new_top_p;
        config_.llm_top_k          = new_top_k;
        config_.llm_min_p          = new_min_p;
        config_.llm_repeat_penalty = new_repeat_penalty;
        config_.llm_frequency_penalty = new_frequency_penalty;
        config_.llm_presence_penalty  = new_presence_penalty;
        config_.exclude_patterns  = new_patterns;
        config_.semantic_search_enabled = new_semantic;
        config_.embedding_model   = new_sem_model;
        config_.reranker_enabled  = new_reranker_enabled;
        config_.reranker_model    = new_reranker_md;
        config_.reranker_top_k    = new_reranker_top_k;
        config_.tool_approval_policies = std::move(new_approvals);
        config_.capabilities      = new_caps;
        config_.memory_enabled    = new_caps.memory_bank;

        spdlog::info("Settings changed (llm={}, index={}, semantic={}, "
                     "approvals={}, capabilities={})",
                     llm_changed_, index_changed_, semantic_changed_,
                     approvals_changed, capabilities_changed_);
    }

    evt.Skip();
}

// ---------------------------------------------------------------------------
// S5.M -- Global defaults snapshot
// ---------------------------------------------------------------------------

WorkspaceConfig SettingsDialog::snapshot_dialog_state() const
{
    // Start from the live workspace config so any fields the dialog doesn't
    // manage (memory.*, agent.*, git.*) round-trip through the global file.
    WorkspaceConfig out = config_;

    out.llm_endpoint      = endpoint_ctrl_->GetValue().ToStdString();
    out.llm_model         = model_ctrl_->GetValue().ToStdString();
    out.llm_temperature   = temperature_ctrl_->GetValue();
    out.llm_context_limit = context_ctrl_->GetValue();
    out.llm_max_tokens    = max_tokens_ctrl_->GetValue();

    if (tool_format_ctrl_) {
        switch (tool_format_ctrl_->GetSelection()) {
            case 0: out.llm_tool_format = "auto";   break;
            case 1: out.llm_tool_format = "openai"; break;
            case 2: out.llm_tool_format = "qwen";   break;
            case 3: out.llm_tool_format = "claude"; break;
            case 4: out.llm_tool_format = "none";   break;
        }
    }

    if (top_p_ctrl_)          out.llm_top_p          = top_p_ctrl_->GetValue();
    if (top_k_ctrl_)          out.llm_top_k          = top_k_ctrl_->GetValue();
    if (min_p_ctrl_)          out.llm_min_p          = min_p_ctrl_->GetValue();
    if (repeat_penalty_ctrl_) out.llm_repeat_penalty = repeat_penalty_ctrl_->GetValue();
    if (frequency_penalty_ctrl_) out.llm_frequency_penalty = frequency_penalty_ctrl_->GetValue();
    if (presence_penalty_ctrl_)  out.llm_presence_penalty  = presence_penalty_ctrl_->GetValue();

    // exclude_patterns -- parse from multi-line text control.
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
        out.exclude_patterns = std::move(patterns);
    }

    out.semantic_search_enabled = semantic_enabled_ctrl_->GetValue();
    out.embedding_model         = semantic_model_ctrl_->GetValue().ToStdString();
    out.reranker_enabled        = reranker_enabled_ctrl_->GetValue();
    out.reranker_model          = reranker_model_ctrl_->GetValue().ToStdString();
    out.reranker_top_k          = reranker_top_k_ctrl_->GetValue();

    if (cap_bg_)       out.capabilities.background_processes = cap_bg_->IsChecked();
    if (cap_semantic_) out.capabilities.semantic_search      = cap_semantic_->IsChecked();
    if (cap_code_)     out.capabilities.code_aware_search    = cap_code_->IsChecked();
    if (cap_memory_)   out.capabilities.memory_bank          = cap_memory_->IsChecked();
    if (cap_web_)      out.capabilities.web_retrieval        = cap_web_->IsChecked();

    // Keep legacy mirrors in sync with the canonical capability bools.
    out.memory_enabled = out.capabilities.memory_bank;

    // Tool approvals -- one entry per per-tool dropdown that differs from the
    // tool's default. mcp:* trust keys deliberately omitted; save_global_config
    // filters them anyway, and the dialog manages them through a separate UI.
    std::unordered_map<std::string, ToolApprovalPolicy> approvals;
    for (size_t i = 0; i < tool_names_.size(); ++i) {
        int sel = approval_choices_[i]->GetSelection();
        ToolApprovalPolicy chosen = ToolApprovalPolicy::ask;
        if (sel == 1) chosen = ToolApprovalPolicy::auto_approve;
        else if (sel == 2) chosen = ToolApprovalPolicy::deny;
        ITool* tool = tools_.find(tool_names_[i]);
        ToolApprovalPolicy default_policy = tool ? tool->approval_policy()
                                                 : ToolApprovalPolicy::ask;
        if (chosen != default_policy)
            approvals[tool_names_[i]] = chosen;
    }
    out.tool_approval_policies = std::move(approvals);

    return out;
}

void SettingsDialog::on_save_as_global_defaults(wxCommandEvent& /*evt*/)
{
    if (wxMessageBox(
            "Save the current Settings values as global defaults?\n\n"
            "These values will be used as the template for any new workspace "
            "you open from this point on. Existing workspaces are not affected.",
            "Save as global defaults",
            wxYES_NO | wxICON_QUESTION, this) != wxYES) {
        return;
    }

    GlobalConfig snap = snapshot_dialog_state();
    if (save_global_config(snap)) {
        wxMessageBox(
            wxString::Format("Saved to %s.",
                wxString::FromUTF8(global_paths::config_path().string())),
            "Locus", wxOK | wxICON_INFORMATION, this);
    } else {
        wxMessageBox("Failed to save global defaults. Check the log for details.",
                     "Locus", wxOK | wxICON_ERROR, this);
    }
}

// ---------------------------------------------------------------------------
// Capabilities tab (S5.A)
// ---------------------------------------------------------------------------

wxPanel* SettingsDialog::build_capabilities_tab(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = panel->GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);

    auto* intro = new wxStaticText(panel, wxID_ANY,
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
        cb = new wxCheckBox(panel, wxID_ANY, full);
        cb->SetValue(initial);
        cb->SetToolTip(tooltip);
        outer->Add(cb, 0, wxLEFT | wxRIGHT | wxTOP, 8);

        auto* hint = new wxStaticText(panel, wxID_ANY, tooltip);
        hint->SetFont(hint_font);
        hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        hint->Wrap(540);
        outer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 26);
    };

    add_row(cap_bg_, "Background processes",
            capability_token_estimates::k_background_processes,
            "Adds run_command_bg, read_process_output, stop_process, "
            "list_processes. Useful for dev servers and long-running jobs.",
            config_.capabilities.background_processes);

    add_row(cap_semantic_, "Semantic search",
            capability_token_estimates::k_semantic_search,
            "Adds `semantic` and `hybrid` modes to the search tool, and "
            "loads the embedding model. Disabling here also disables the "
            "Index-tab semantic toggle.",
            config_.capabilities.semantic_search);

    add_row(cap_code_, "Code-aware search",
            capability_token_estimates::k_code_aware_search,
            "Adds `symbols` and `ast` modes to the search tool, plus the "
            "get_file_outline tool. Skip for text-only workspaces.",
            config_.capabilities.code_aware_search);

    add_row(cap_memory_, "Memory bank",
            capability_token_estimates::k_memory_bank,
            "Adds add_memory + search_memory tools, /memorize and /forget "
            "slash commands, and the system-prompt memory slot.",
            config_.capabilities.memory_bank);

    add_row(cap_web_, "Web retrieval",
            capability_token_estimates::k_web_retrieval,
            "Adds web fetch + search tools when M6 web RAG ships. Currently "
            "a placeholder -- no tools register against this bucket yet.",
            config_.capabilities.web_retrieval);

    panel->SetSizer(outer);
    return panel;
}

} // namespace locus
