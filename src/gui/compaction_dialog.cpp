#include "compaction_dialog.h"

#include <algorithm>

namespace locus {

enum {
    ID_RADIO_B = wxID_HIGHEST + 400,
    ID_RADIO_C,
    ID_TURNS_SLIDER,
};

CompactionDialog::CompactionDialog(wxWindow* parent,
                                   int used_tokens,
                                   int limit_tokens,
                                   const ConversationHistory& history)
    : wxDialog(parent, wxID_ANY, "Compact Context",
               wxDefaultPosition, wxSize(520, 480),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , history_(history)
    , used_tokens_(used_tokens)
    , limit_tokens_(limit_tokens)
{
    create_controls(used_tokens, limit_tokens);
    layout();

    update_preview();
    update_token_counts();

    Centre();
}

void CompactionDialog::create_controls(int used_tokens, int limit_tokens)
{
    int pct = (limit_tokens > 0) ? (used_tokens * 100 / limit_tokens) : 0;

    // Header.
    before_label_ = new wxStaticText(this, wxID_ANY,
        wxString::Format("Current: %d / %d tokens (%d%%)",
                         used_tokens, limit_tokens, pct));
    auto bold_font = before_label_->GetFont();
    bold_font.SetWeight(wxFONTWEIGHT_BOLD);
    before_label_->SetFont(bold_font);

    after_label_ = new wxStaticText(this, wxID_ANY, "After compaction: --");
    freed_label_ = new wxStaticText(this, wxID_ANY, "Freed: --");
    freed_label_->SetForegroundColour(wxColour(76, 175, 80));

    // Strategy B: drop tool results.
    radio_b_ = new wxRadioButton(this, ID_RADIO_B,
        "Drop tool results  (keep conversation flow, strip tool output)",
        wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    radio_b_->SetValue(true);

    // Strategy C: drop oldest turns.
    radio_c_ = new wxRadioButton(this, ID_RADIO_C,
        "Drop oldest turns  (remove N oldest exchanges)");

    // Turns slider (for strategy C).
    int max_turns = std::max(1, static_cast<int>(history_.size()) / 2);
    turns_slider_ = new wxSlider(this, ID_TURNS_SLIDER, 3, 1,
                                 std::max(1, max_turns),
                                 wxDefaultPosition, wxDefaultSize,
                                 wxSL_HORIZONTAL | wxSL_LABELS);
    turns_slider_->Enable(false);  // disabled until radio C is selected

    // Preview list (for strategy C).
    preview_list_ = new wxListBox(this, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 120));
    preview_list_->Enable(false);

    // Buttons.
    btn_ok_ = new wxButton(this, wxID_OK, "Compact");
    btn_cancel_ = new wxButton(this, wxID_CANCEL, "Cancel");

    // Events.
    radio_b_->Bind(wxEVT_RADIOBUTTON, &CompactionDialog::on_strategy_changed, this);
    radio_c_->Bind(wxEVT_RADIOBUTTON, &CompactionDialog::on_strategy_changed, this);
    turns_slider_->Bind(wxEVT_SLIDER, &CompactionDialog::on_slider_changed, this);
    btn_ok_->Bind(wxEVT_BUTTON, &CompactionDialog::on_ok, this);
}

void CompactionDialog::layout()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    sizer->Add(before_label_, 0, wxEXPAND | wxALL, 10);

    // Strategy B.
    sizer->Add(radio_b_, 0, wxLEFT | wxRIGHT | wxTOP, 10);
    sizer->AddSpacer(8);

    // Strategy C.
    sizer->Add(radio_c_, 0, wxLEFT | wxRIGHT, 10);

    auto* c_box = new wxStaticBoxSizer(wxVERTICAL, this, "Turns to drop");
    c_box->Add(turns_slider_, 0, wxEXPAND | wxALL, 4);
    c_box->Add(preview_list_, 1, wxEXPAND | wxALL, 4);
    sizer->Add(c_box, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    sizer->AddSpacer(8);

    // Token counts.
    auto* counts = new wxBoxSizer(wxHORIZONTAL);
    counts->Add(after_label_, 1, wxALIGN_CENTER_VERTICAL);
    counts->Add(freed_label_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    sizer->Add(counts, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    sizer->AddSpacer(8);

    // Buttons.
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(btn_cancel_, 0, wxRIGHT, 4);
    btn_sizer->Add(btn_ok_, 0);
    sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

    SetSizer(sizer);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void CompactionDialog::on_strategy_changed(wxCommandEvent& /*evt*/)
{
    bool is_c = radio_c_->GetValue();
    turns_slider_->Enable(is_c);
    preview_list_->Enable(is_c);

    update_preview();
    update_token_counts();
}

void CompactionDialog::on_slider_changed(wxCommandEvent& /*evt*/)
{
    update_preview();
    update_token_counts();
}

void CompactionDialog::on_ok(wxCommandEvent& /*evt*/)
{
    choice_.made = true;
    if (radio_b_->GetValue()) {
        choice_.strategy = CompactionStrategy::drop_tool_results;
    } else {
        choice_.strategy = CompactionStrategy::drop_oldest;
        choice_.drop_n = turns_slider_->GetValue();
    }
    EndModal(wxID_OK);
}

// ---------------------------------------------------------------------------
// Preview and token estimates
// ---------------------------------------------------------------------------

void CompactionDialog::update_preview()
{
    preview_list_->Clear();

    if (!radio_c_->GetValue()) return;

    int n = turns_slider_->GetValue();
    auto& msgs = history_.messages();

    // Show which messages would be dropped (skip system message at index 0).
    int dropped = 0;
    for (size_t i = 1; i < msgs.size() && dropped < n * 2; ++i) {
        auto& m = msgs[i];
        if (m.role == MessageRole::system) continue;

        wxString role = wxString::FromUTF8(to_string(m.role));
        wxString preview = wxString::FromUTF8(m.content.substr(0, 60));
        if (m.content.size() > 60) preview += "...";

        // Replace newlines for single-line display.
        preview.Replace("\n", " ");

        preview_list_->Append(wxString::Format("[%s] %s", role, preview));
        ++dropped;
    }
}

void CompactionDialog::update_token_counts()
{
    int freed = estimate_freed_tokens();
    int after = std::max(0, used_tokens_ - freed);
    int after_pct = (limit_tokens_ > 0) ? (after * 100 / limit_tokens_) : 0;

    after_label_->SetLabel(
        wxString::Format("After: ~%d tokens (%d%%)", after, after_pct));
    freed_label_->SetLabel(
        wxString::Format("Freed: ~%d tokens", freed));
}

int CompactionDialog::estimate_freed_tokens() const
{
    auto& msgs = history_.messages();

    if (radio_b_->GetValue()) {
        // Strategy B: estimate tokens in tool-result messages.
        int freed = 0;
        for (auto& m : msgs) {
            if (m.role == MessageRole::tool && !m.content.empty())
                freed += ILLMClient::estimate_tokens(m.content);
        }
        return freed;
    }

    // Strategy C: estimate tokens in the N oldest turns.
    int n = turns_slider_->GetValue();
    int freed = 0;
    int dropped = 0;
    for (size_t i = 1; i < msgs.size() && dropped < n * 2; ++i) {
        if (msgs[i].role == MessageRole::system) continue;
        freed += ILLMClient::estimate_tokens(msgs[i].content);
        ++dropped;
    }
    return freed;
}

} // namespace locus
