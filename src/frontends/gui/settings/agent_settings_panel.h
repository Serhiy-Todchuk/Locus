#pragma once

#include "settings_panel.h"

#include <wx/wx.h>

namespace locus {

// S6.18 C.1 + C.2 -- Settings > Agent tab. Houses two named-preset wxChoice
// controls that map onto multiple underlying knobs:
//
//   compaction.aggressiveness: Gentle / Balanced / Aggressive / Custom
//     -> drives layer_drop_redundant / strip_large / drop_old_reasoning /
//        drop_oldest_turns / llm_summary at config-apply time via the
//        existing layers_for_aggressiveness helper.
//
//   agent.prompt_cost: Minimal / Balanced / Verbose / Default (auto)
//     -> drives lazy_tool_manifest + system_prompt_profile via the existing
//        prompt_cost_apply helper.
//
// The underlying booleans / strings still round-trip through config.json --
// the wxChoice widgets are a curated UI on top of the same knobs. Each
// control exposes the named preset as the labelled choice value; commit
// writes the preset string back to its WorkspaceConfig field and lets the
// downstream helpers expand it.
class AgentSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    AgentSettingsPanel(wxWindow* parent, const WorkspaceConfig& config);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

private:
    wxChoice* aggressiveness_ctrl_ = nullptr;
    wxChoice* prompt_cost_ctrl_   = nullptr;
};

} // namespace locus
