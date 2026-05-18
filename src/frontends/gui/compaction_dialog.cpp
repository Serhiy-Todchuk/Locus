#include "compaction_dialog.h"

#include "locus_accessible.h"
#include "ui_names.h"

#include "../../llm/token_counter.h"

#include <algorithm>
#include <sstream>

namespace locus {

enum {
    ID_LAYER1 = wxID_HIGHEST + 400,
    ID_LAYER2,
    ID_LAYER3,
    ID_LAYER5,
    ID_LAYER6,
    ID_STRIP_THRESHOLD,
    ID_OLDER_THAN,
    ID_KEEP_RECENT,
    ID_SUMMARY_TOKENS,
};

CompactionDialog::CompactionDialog(wxWindow* parent,
                                   int used_tokens,
                                   int limit_tokens,
                                   const ConversationHistory& history,
                                   const WorkspaceConfig::Compaction& cfg)
    : wxDialog(parent, wxID_ANY, "Compact Context",
               wxDefaultPosition, wxSize(620, 620),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , history_(history)
    , cfg_(cfg)
    , used_tokens_(used_tokens)
    , limit_tokens_(limit_tokens)
{
    SetName(ui_names::kCompactionDialog);
    gui::apply_locus_accessible_name(this);

    create_controls(used_tokens, limit_tokens);
    layout();
    update_preview();

    Centre();
}

void CompactionDialog::create_controls(int used_tokens, int limit_tokens)
{
    int pct = (limit_tokens > 0) ? (used_tokens * 100 / limit_tokens) : 0;

    before_label_ = new wxStaticText(this, wxID_ANY,
        wxString::Format("Current: %d / %d tokens (%d%%)",
                         used_tokens, limit_tokens, pct));
    auto bold_font = before_label_->GetFont();
    bold_font.SetWeight(wxFONTWEIGHT_BOLD);
    before_label_->SetFont(bold_font);

    after_label_   = new wxStaticText(this, wxID_ANY, "After:  --");
    freed_label_   = new wxStaticText(this, wxID_ANY, "Freed:  --");
    freed_label_->SetForegroundColour(wxColour(76, 175, 80));
    layer_summary_ = new wxStaticText(this, wxID_ANY, "");

    // Per-layer checkboxes. Initial state mirrors the saved auto-compact
    // cascade (config-driven so the Save button round-trips); defaults if
    // never saved match the original hardcoded auto cascade 1+2+3+6.
    cb_layer1_ = new wxCheckBox(this, ID_LAYER1,
        "Drop redundant tool results  (None)");
    cb_layer1_->SetValue(cfg_.layer_drop_redundant_tool_results);
    cb_layer1_->SetName(ui_names::kCompactionStrategyB);  // re-use existing automation id
    gui::apply_locus_accessible_name(cb_layer1_);

    cb_layer2_ = new wxCheckBox(this, ID_LAYER2,
        "Strip large tool bodies  (Low -- header retained)");
    cb_layer2_->SetValue(cfg_.layer_strip_large_tool_bodies);

    cb_layer3_ = new wxCheckBox(this, ID_LAYER3,
        "Drop reasoning from older turns  (None for replay)");
    cb_layer3_->SetValue(cfg_.layer_drop_old_reasoning);

    cb_layer5_ = new wxCheckBox(this, ID_LAYER5,
        "Drop oldest turn pairs  (Mechanical -- escalation only)");
    cb_layer5_->SetValue(cfg_.layer_drop_oldest_turns);
    cb_layer5_->SetName(ui_names::kCompactionStrategyC);
    gui::apply_locus_accessible_name(cb_layer5_);

    cb_layer6_ = new wxCheckBox(this, ID_LAYER6,
        "LLM summary of dropped span  (Lossy; costs one LLM call)");
    cb_layer6_->SetValue(cfg_.layer_llm_summary);

    // Per-layer knobs.
    sp_strip_threshold_ = new wxSpinCtrl(this, ID_STRIP_THRESHOLD, "",
        wxDefaultPosition, wxSize(90, -1), wxSP_ARROW_KEYS,
        50, 16000, cfg_.strip_threshold_tokens);
    sp_older_than_turns_ = new wxSpinCtrl(this, ID_OLDER_THAN, "",
        wxDefaultPosition, wxSize(90, -1), wxSP_ARROW_KEYS,
        0, 50, cfg_.older_than_turns);
    sp_keep_recent_ = new wxSpinCtrl(this, ID_KEEP_RECENT, "",
        wxDefaultPosition, wxSize(90, -1), wxSP_ARROW_KEYS,
        0, 50, cfg_.keep_recent_turns);
    sp_keep_recent_->SetName(ui_names::kCompactionTurnsSlider);
    gui::apply_locus_accessible_name(sp_keep_recent_);

    sp_summary_tokens_ = new wxSpinCtrl(this, ID_SUMMARY_TOKENS, "",
        wxDefaultPosition, wxSize(90, -1), wxSP_ARROW_KEYS,
        128, 8192, cfg_.summary_max_tokens);

    tx_custom_instructions_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(cfg_.custom_summary_instructions),
        wxDefaultPosition, wxSize(-1, 60), wxTE_MULTILINE);
    tx_custom_instructions_->SetName(ui_names::kCompactionPreviewList);
    gui::apply_locus_accessible_name(tx_custom_instructions_);

    btn_ok_     = new wxButton(this, wxID_OK, "Compact");
    btn_save_   = new wxButton(this, wxID_ANY, "Save");
    btn_save_->SetToolTip(
        "Save current selection as auto-compact defaults. "
        "Does not run compaction now.");
    btn_cancel_ = new wxButton(this, wxID_CANCEL, "Cancel");

    auto rebind = [&](wxWindow* w) {
        if (auto* cb = dynamic_cast<wxCheckBox*>(w))
            cb->Bind(wxEVT_CHECKBOX, &CompactionDialog::on_any_changed, this);
        if (auto* sp = dynamic_cast<wxSpinCtrl*>(w))
            sp->Bind(wxEVT_SPINCTRL, &CompactionDialog::on_any_changed, this);
    };
    rebind(cb_layer1_); rebind(cb_layer2_); rebind(cb_layer3_);
    rebind(cb_layer5_); rebind(cb_layer6_);
    rebind(sp_strip_threshold_); rebind(sp_older_than_turns_);
    rebind(sp_keep_recent_); rebind(sp_summary_tokens_);

    btn_ok_->Bind(wxEVT_BUTTON, &CompactionDialog::on_ok, this);
    btn_save_->Bind(wxEVT_BUTTON, &CompactionDialog::on_save, this);
}

void CompactionDialog::layout()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(before_label_, 0, wxEXPAND | wxALL, 10);

    auto* layers = new wxStaticBoxSizer(wxVERTICAL, this, "Layers");
    layers->Add(cb_layer1_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    layers->Add(cb_layer2_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(24);
        row->Add(new wxStaticText(this, wxID_ANY,
                                  "Strip threshold (tokens):"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(sp_strip_threshold_, 0, wxALIGN_CENTER_VERTICAL);
        layers->Add(row, 0, wxLEFT | wxRIGHT, 6);
    }
    layers->Add(cb_layer3_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(24);
        row->Add(new wxStaticText(this, wxID_ANY,
                                  "Older than (turns):"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(sp_older_than_turns_, 0, wxALIGN_CENTER_VERTICAL);
        layers->Add(row, 0, wxLEFT | wxRIGHT, 6);
    }
    layers->Add(cb_layer5_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(24);
        row->Add(new wxStaticText(this, wxID_ANY, "Keep recent (turns):"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(sp_keep_recent_, 0, wxALIGN_CENTER_VERTICAL);
        layers->Add(row, 0, wxLEFT | wxRIGHT, 6);
    }
    layers->Add(cb_layer6_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(24);
        row->Add(new wxStaticText(this, wxID_ANY,
                                  "Summary max tokens:"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(sp_summary_tokens_, 0, wxALIGN_CENTER_VERTICAL);
        layers->Add(row, 0, wxLEFT | wxRIGHT, 6);
    }
    sizer->Add(layers, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    sizer->AddSpacer(8);

    auto* instr = new wxStaticBoxSizer(wxVERTICAL, this,
        "Custom summary instructions (Pi-style; applied to layer 6 this run)");
    instr->Add(tx_custom_instructions_, 1, wxEXPAND | wxALL, 4);
    sizer->Add(instr, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    sizer->AddSpacer(8);

    auto* preview = new wxStaticBoxSizer(wxVERTICAL, this, "Preview");
    auto* counts = new wxBoxSizer(wxHORIZONTAL);
    counts->Add(after_label_, 1, wxALIGN_CENTER_VERTICAL);
    counts->Add(freed_label_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    preview->Add(counts, 0, wxEXPAND | wxALL, 4);
    preview->Add(layer_summary_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    sizer->Add(preview, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    sizer->AddSpacer(8);

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(btn_cancel_, 0, wxRIGHT, 4);
    btn_sizer->Add(btn_save_,   0, wxRIGHT, 4);
    btn_sizer->Add(btn_ok_, 0);
    sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

    SetSizer(sizer);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void CompactionDialog::on_any_changed(wxCommandEvent& /*evt*/)
{
    update_preview();
}

void CompactionDialog::on_ok(wxCommandEvent& /*evt*/)
{
    choice_.made                = true;
    choice_.save_as_default     = false;
    choice_.selection           = snapshot_selection();
    choice_.custom_instructions = tx_custom_instructions_
        ? tx_custom_instructions_->GetValue().ToStdString()
        : std::string{};
    EndModal(wxID_OK);
}

void CompactionDialog::on_save(wxCommandEvent& /*evt*/)
{
    // Same snapshot as Compact, but the caller persists it to
    // WorkspaceConfig::Compaction and does NOT run compaction.
    choice_.made                = false;
    choice_.save_as_default     = true;
    choice_.selection           = snapshot_selection();
    choice_.custom_instructions = tx_custom_instructions_
        ? tx_custom_instructions_->GetValue().ToStdString()
        : std::string{};
    EndModal(wxID_OK);
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

CompactionLayerSelection CompactionDialog::snapshot_selection() const
{
    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = cb_layer1_ && cb_layer1_->GetValue();
    sel.strip_large_tool_bodies     = cb_layer2_ && cb_layer2_->GetValue();
    sel.drop_old_reasoning          = cb_layer3_ && cb_layer3_->GetValue();
    sel.drop_oldest_turns           = cb_layer5_ && cb_layer5_->GetValue();
    sel.llm_summary                 = cb_layer6_ && cb_layer6_->GetValue();
    sel.strip_threshold_tokens      = sp_strip_threshold_
        ? sp_strip_threshold_->GetValue() : cfg_.strip_threshold_tokens;
    sel.older_than_turns            = sp_older_than_turns_
        ? sp_older_than_turns_->GetValue() : cfg_.older_than_turns;
    sel.keep_recent_turns           = sp_keep_recent_
        ? sp_keep_recent_->GetValue() : cfg_.keep_recent_turns;
    sel.summary_max_tokens          = sp_summary_tokens_
        ? sp_summary_tokens_->GetValue() : cfg_.summary_max_tokens;
    sel.preserve_short_user_msgs_max_tokens   = cfg_.preserve_short_user_msgs_max_tokens;
    sel.preserve_short_tool_calls_max_tokens  = cfg_.preserve_short_tool_calls_max_tokens;
    return sel;
}

void CompactionDialog::update_preview()
{
    if (!after_label_) return;

    auto sel = snapshot_selection();
    int freed = 0;
    int layer_lines = 0;
    std::ostringstream layer_text;

    if (sel.drop_redundant_tool_results) {
        // Quick: count duplicate tool results.
        ++layer_lines;
        layer_text << "Layer 1: drop redundant tool results -- "
                   << "(applies if duplicates exist)\n";
    }
    if (sel.strip_large_tool_bodies) {
        int local = 0;
        for (auto& m : history_.messages()) {
            if (m.role == MessageRole::tool && !m.content.empty()) {
                int t = TokenCounter::estimate(m.content);
                if (t > sel.strip_threshold_tokens)
                    local += (t - 30);  // header keeps a few tokens
            }
        }
        freed += local;
        ++layer_lines;
        layer_text << "Layer 2: strip tool bodies > "
                   << sel.strip_threshold_tokens << " tokens -> ~"
                   << local << " freed\n";
    }
    if (sel.drop_old_reasoning) {
        int local = 0;
        for (auto& m : history_.messages()) {
            if (m.role != MessageRole::assistant) continue;
            auto p = m.content.find("<think>");
            if (p == std::string::npos) continue;
            auto e = m.content.find("</think>", p);
            if (e == std::string::npos) continue;
            local += TokenCounter::estimate(m.content.substr(p, e - p + 8));
        }
        freed += local;
        ++layer_lines;
        layer_text << "Layer 3: drop <think> blocks -> ~" << local << " freed\n";
    }
    if (sel.drop_oldest_turns) {
        auto cands = CompactionPipeline::drop_candidates(history_.messages(), sel);
        int local = 0;
        for (const auto& s : cands) {
            for (auto i = s.begin; i < s.end; ++i)
                local += TokenCounter::estimate_message(history_.messages()[i]);
        }
        freed += local;
        ++layer_lines;
        layer_text << "Layer 5: drop oldest turn pairs -> ~"
                   << local << " freed (" << cands.size() << " span(s))\n";
    }
    if (sel.llm_summary) {
        ++layer_lines;
        layer_text << "Layer 6: LLM summary -> replaces dropped span with one "
                   << "assistant message (cost ~"
                   << sel.summary_max_tokens << " tokens)\n";
    }
    (void)layer_lines;

    int after_total = std::max(0, used_tokens_ - freed);
    int after_pct = (limit_tokens_ > 0) ? (after_total * 100 / limit_tokens_) : 0;

    after_label_->SetLabel(
        wxString::Format("After:  ~%d tokens (%d%%)", after_total, after_pct));
    freed_label_->SetLabel(
        wxString::Format("Freed:  ~%d tokens", freed));
    layer_summary_->SetLabel(wxString::FromUTF8(layer_text.str()));
    Layout();
}

} // namespace locus
