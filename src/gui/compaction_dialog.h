#pragma once

#include "../conversation.h"
#include "../frontend.h"

#include <wx/wx.h>
#include <wx/listbox.h>
#include <wx/slider.h>

#include <string>

namespace locus {

// Result of the compaction dialog: which strategy the user chose and how many
// turns to drop (for Strategy C).
struct CompactionChoice {
    bool made = false;                     // false if user cancelled
    CompactionStrategy strategy = CompactionStrategy::drop_tool_results;
    int  drop_n = 3;                       // only for drop_oldest
};

// Modal dialog shown when context is critically full or when the user
// manually triggers compaction. Offers Strategy B (drop tool results) and
// Strategy C (drop N oldest turns with preview).
//
// Strategy A (LLM summary) is listed but not yet functional — it requires
// an async LLM call which will be wired up in a future stage.
class CompactionDialog : public wxDialog {
public:
    CompactionDialog(wxWindow* parent,
                     int used_tokens,
                     int limit_tokens,
                     const ConversationHistory& history);

    CompactionChoice result() const { return choice_; }

private:
    void create_controls(int used_tokens, int limit_tokens);
    void layout();

    // Event handlers.
    void on_strategy_changed(wxCommandEvent& evt);
    void on_slider_changed(wxCommandEvent& evt);
    void on_ok(wxCommandEvent& evt);

    void update_preview();
    void update_token_counts();

    // Estimate tokens that would be freed by the selected strategy.
    int estimate_freed_tokens() const;

    const ConversationHistory& history_;
    CompactionChoice choice_;

    // Controls.
    wxRadioButton* radio_b_       = nullptr;  // drop tool results
    wxRadioButton* radio_c_       = nullptr;  // drop oldest turns
    wxSlider*      turns_slider_  = nullptr;
    wxListBox*     preview_list_  = nullptr;
    wxStaticText*  before_label_  = nullptr;
    wxStaticText*  after_label_   = nullptr;
    wxStaticText*  freed_label_   = nullptr;
    wxButton*      btn_ok_        = nullptr;
    wxButton*      btn_cancel_    = nullptr;

    int used_tokens_  = 0;
    int limit_tokens_ = 0;
};

} // namespace locus
