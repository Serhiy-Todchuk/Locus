#include "agent_settings_panel.h"

#include "../ui_names.h"
#include "../locus_accessible.h"

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace locus {

namespace {

// Aggressiveness preset table -- (display label, config key).
struct PresetRow {
    const wxString label;
    const char*    key;
};

const PresetRow k_aggressiveness_rows[] = {
    {"Gentle (drop redundant tool results + strip large bodies)",   "gentle"},
    {"Balanced (+ drop old reasoning + LLM summary) [default]",     "balanced"},
    {"Aggressive (+ drop oldest turns)",                             "aggressive"},
    {"Custom (use individual layer toggles in .locus/config.json)",  "custom"},
};

const PresetRow k_prompt_cost_rows[] = {
    {"Minimal (lazy manifest + minimal prose)",                      "minimal"},
    {"Balanced (lazy manifest + compact prose)",                     "balanced"},
    {"Verbose (full manifest + full prose)",                         "verbose"},
    {"Default (auto: lazy manifest + full prose)",                   "default"},
};

int row_index_for(const PresetRow* rows, std::size_t n, const std::string& key,
                  int fallback)
{
    for (std::size_t i = 0; i < n; ++i) {
        if (key == rows[i].key) return static_cast<int>(i);
    }
    return fallback;
}

} // namespace

AgentSettingsPanel::AgentSettingsPanel(wxWindow* parent,
                                      const WorkspaceConfig& config)
    : wxPanel(parent)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);

    auto* intro = new wxStaticText(this, wxID_ANY,
        "Per-workspace agent behaviour. Each chooser maps onto multiple "
        "underlying knobs; pick \"Custom\" or \"Default\" to fall back to the "
        "individual flags in .locus/config.json.");
    intro->Wrap(560);
    outer->Add(intro, 0, wxALL, 8);

    // -- Compaction aggressiveness -------------------------------------------
    {
        outer->Add(new wxStaticText(this, wxID_ANY, "Compaction aggressiveness:"),
                   0, wxLEFT | wxRIGHT | wxTOP, 8);
        aggressiveness_ctrl_ = new wxChoice(this, wxID_ANY);
        for (const auto& row : k_aggressiveness_rows)
            aggressiveness_ctrl_->Append(row.label);
        aggressiveness_ctrl_->SetSelection(
            row_index_for(k_aggressiveness_rows,
                          sizeof(k_aggressiveness_rows) / sizeof(k_aggressiveness_rows[0]),
                          config.compaction.aggressiveness,
                          /*fallback=*/1));  // balanced
        aggressiveness_ctrl_->SetName(ui_names::kSettingsCompactionAggressiveness);
        aggressiveness_ctrl_->SetToolTip(
            "How aggressively the auto-compaction pipeline trims older "
            "history when the context budget fills up.");
        gui::apply_locus_accessible_name(aggressiveness_ctrl_);
        outer->Add(aggressiveness_ctrl_, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
        auto* hint = new wxStaticText(this, wxID_ANY,
            "Custom honours the individual layer_* flags in .locus/config.json. "
            "All other presets ignore those flags and apply a fixed layer set.");
        hint->SetFont(hint_font);
        hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        hint->Wrap(540);
        outer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    }

    // -- Prompt cost ---------------------------------------------------------
    {
        outer->Add(new wxStaticText(this, wxID_ANY, "Prompt cost:"),
                   0, wxLEFT | wxRIGHT | wxTOP, 8);
        prompt_cost_ctrl_ = new wxChoice(this, wxID_ANY);
        for (const auto& row : k_prompt_cost_rows)
            prompt_cost_ctrl_->Append(row.label);
        // Default fallback row when the config field is empty: the
        // "Default (auto)" entry preserves the pre-S6.17 behaviour.
        int default_idx = (sizeof(k_prompt_cost_rows) / sizeof(k_prompt_cost_rows[0])) - 1;
        prompt_cost_ctrl_->SetSelection(
            row_index_for(k_prompt_cost_rows,
                          sizeof(k_prompt_cost_rows) / sizeof(k_prompt_cost_rows[0]),
                          config.agent.prompt_cost,
                          default_idx));
        prompt_cost_ctrl_->SetName(ui_names::kSettingsAgentPromptCost);
        prompt_cost_ctrl_->SetToolTip(
            "How much per-turn prompt overhead to spend on tool schemas + "
            "system-prompt prose. Smaller = more headroom for tool results; "
            "larger = more guidance to the model.");
        gui::apply_locus_accessible_name(prompt_cost_ctrl_);
        outer->Add(prompt_cost_ctrl_, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
        auto* hint = new wxStaticText(this, wxID_ANY,
            "Sets lazy_tool_manifest + system_prompt_profile in tandem. "
            "Pick Default for the small-LLM-friendly auto default (lazy "
            "manifest + full prose).");
        hint->SetFont(hint_font);
        hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        hint->Wrap(540);
        outer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    }

    outer->AddStretchSpacer(1);
    SetSizer(outer);
}

void AgentSettingsPanel::load_from_config(const WorkspaceConfig& cfg)
{
    if (aggressiveness_ctrl_) {
        aggressiveness_ctrl_->SetSelection(
            row_index_for(k_aggressiveness_rows,
                          sizeof(k_aggressiveness_rows) / sizeof(k_aggressiveness_rows[0]),
                          cfg.compaction.aggressiveness, 1));
    }
    if (prompt_cost_ctrl_) {
        int default_idx = (sizeof(k_prompt_cost_rows) / sizeof(k_prompt_cost_rows[0])) - 1;
        prompt_cost_ctrl_->SetSelection(
            row_index_for(k_prompt_cost_rows,
                          sizeof(k_prompt_cost_rows) / sizeof(k_prompt_cost_rows[0]),
                          cfg.agent.prompt_cost, default_idx));
    }
}

bool AgentSettingsPanel::validate(wxString& /*out_error*/) const
{
    return true;
}

void AgentSettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    if (aggressiveness_ctrl_) {
        int idx = aggressiveness_ctrl_->GetSelection();
        if (idx >= 0
            && static_cast<size_t>(idx)
               < sizeof(k_aggressiveness_rows) / sizeof(k_aggressiveness_rows[0])) {
            cfg.compaction.aggressiveness = k_aggressiveness_rows[idx].key;
        }
    }
    if (prompt_cost_ctrl_) {
        int idx = prompt_cost_ctrl_->GetSelection();
        if (idx >= 0
            && static_cast<size_t>(idx)
               < sizeof(k_prompt_cost_rows) / sizeof(k_prompt_cost_rows[0])) {
            cfg.agent.prompt_cost = k_prompt_cost_rows[idx].key;
        }
    }
}

} // namespace locus
