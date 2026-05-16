#pragma once

#include "../../agent/compaction_pipeline.h"
#include "../../agent/conversation.h"
#include "../../core/frontend.h"
#include "../../core/workspace.h"

#include <wx/wx.h>
#include <wx/checkbox.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <string>

namespace locus {

// S5.F -- Result of the compaction dialog: which layers the user enabled
// and the per-layer knobs. AgentCore receives this via `run_compaction(...)`
// and runs the same cascade auto-compact would.
struct CompactionChoice {
    bool made = false;                       // false if user cancelled
    CompactionLayerSelection selection;
    std::string custom_instructions;         // per-run override

    // Pre-S5.F shim fields, kept so any test or transitional caller that
    // still constructs a CompactionChoice with the legacy strategy/drop_n
    // signature compiles. AgentCore::compact_context() honours these only
    // when `selection` is left at its defaults (i.e. all layers on).
    CompactionStrategy strategy = CompactionStrategy::drop_tool_results;
    int  drop_n = 3;
};

// Modal dialog shown when the user manually triggers compaction. Body is
// per-layer checkboxes + inline knobs + live preview of tokens freed.
class CompactionDialog : public wxDialog {
public:
    CompactionDialog(wxWindow* parent,
                     int used_tokens,
                     int limit_tokens,
                     const ConversationHistory& history,
                     const WorkspaceConfig::Compaction& cfg);

    CompactionChoice result() const { return choice_; }

private:
    void create_controls(int used_tokens, int limit_tokens);
    void layout();

    void on_any_changed(wxCommandEvent& evt);
    void on_ok(wxCommandEvent& evt);

    void update_preview();
    CompactionLayerSelection snapshot_selection() const;

    const ConversationHistory&         history_;
    WorkspaceConfig::Compaction        cfg_;
    CompactionChoice                   choice_;

    // Per-layer checkboxes.
    wxCheckBox* cb_layer1_       = nullptr;  // drop redundant tool results
    wxCheckBox* cb_layer2_       = nullptr;  // strip large tool bodies
    wxCheckBox* cb_layer3_       = nullptr;  // drop old reasoning
    wxCheckBox* cb_layer5_       = nullptr;  // drop oldest turns (escalation)
    wxCheckBox* cb_layer6_       = nullptr;  // LLM summary

    // Per-layer knobs.
    wxSpinCtrl* sp_strip_threshold_  = nullptr;
    wxSpinCtrl* sp_older_than_turns_ = nullptr;
    wxSpinCtrl* sp_keep_recent_      = nullptr;
    wxSpinCtrl* sp_summary_tokens_   = nullptr;

    wxTextCtrl* tx_custom_instructions_ = nullptr;

    wxStaticText* before_label_  = nullptr;
    wxStaticText* after_label_   = nullptr;
    wxStaticText* freed_label_   = nullptr;
    wxStaticText* layer_summary_ = nullptr;
    wxButton*     btn_ok_        = nullptr;
    wxButton*     btn_cancel_    = nullptr;

    int used_tokens_  = 0;
    int limit_tokens_ = 0;
};

} // namespace locus
