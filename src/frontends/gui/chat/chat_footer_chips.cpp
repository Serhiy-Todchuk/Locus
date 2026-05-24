#include "chat_footer_chips.h"

#include "../locus_accessible.h"
#include "../ui_names.h"

#include <algorithm>

namespace locus {

namespace {

wxString format_plan_chip(int done, int total, const std::string& label)
{
    wxString s = wxString::Format("Plan: %d/%d", done, total);
    if (!label.empty()) {
        std::string head = label;
        constexpr size_t k_max = 60;
        if (head.size() > k_max) head = head.substr(0, k_max) + "...";
        s += " - " + wxString::FromUTF8(head);
    }
    return s;
}

} // namespace

ChatFooterChips::ChatFooterChips(wxWindow* parent)
{
    ctx_label_     = new wxStaticText(parent, wxID_ANY, "ctx: 0/0");
    round_chip_    = new wxStaticText(parent, wxID_ANY, "");
    // Compacted button: small, fixed-ish width. wxBU_EXACTFIT so it sizes to
    // the digit string without native button padding bloating it. Initial
    // size hint keeps it small when first surfaced (visibility flip in
    // set_compacted_count will Show/Hide; we don't want a tiny gap when
    // hidden, so it starts Hidden).
    compacted_btn_ = new wxButton(parent, wxID_ANY, "0",
                                  wxDefaultPosition, wxSize(36, -1),
                                  wxBU_EXACTFIT);
    round_chip_->Hide();
    compacted_btn_->Hide();

    ctx_label_->SetName(ui_names::kChatCtxLabel);
    compacted_btn_->SetName(ui_names::kChatCompactedChip);
    round_chip_->SetName(ui_names::kChatRoundChip);
    gui::apply_locus_accessible_name(ctx_label_);
    gui::apply_locus_accessible_name(compacted_btn_);
    gui::apply_locus_accessible_name(round_chip_);
}

bool ChatFooterChips::set_context_meter(int used, int limit,
                                         int prompt_tokens, int completion_tokens,
                                         int reserve_tokens)
{
    last_ctx_used_       = used;
    last_ctx_limit_      = limit;
    last_ctx_prompt_     = prompt_tokens;
    last_ctx_completion_ = completion_tokens;
    last_ctx_reserve_    = reserve_tokens;
    if (completion_tokens > 0) live_completion_estimate_ = 0;
    refresh_ctx_label();
    return false;  // label width is fixed enough that no re-layout is needed
}

void ChatFooterChips::set_generation_progress(int /*chars*/, int est_tokens)
{
    live_completion_estimate_ = std::max(0, est_tokens);
    refresh_ctx_label();
}

void ChatFooterChips::clear_live_estimate()
{
    live_completion_estimate_ = 0;
    refresh_ctx_label();
}

bool ChatFooterChips::on_auto_commit(const wxString& short_sha,
                                      const wxString& branch,
                                      const wxString& subject)
{
    if (short_sha.empty()) return false;
    commit_short_sha_ = short_sha;
    commit_branch_    = branch;
    commit_subject_   = subject;
    return true;  // caller should refresh the status-bar field
}

bool ChatFooterChips::set_round_progress(int round, int max_rounds)
{
    if (!round_chip_) return false;
    if (round <= 0) return hide_round_progress();
    wxString label = (max_rounds > 0)
        ? wxString::Format("round %d/%d", round, max_rounds)
        : wxString::Format("round %d", round);
    round_chip_->SetLabel(label);
    round_chip_->SetToolTip(
        max_rounds > 0
            ? wxString::Format("Agent is on tool-call round %d of %d.\n"
                               "Raise agent.max_rounds_per_message in "
                               ".locus/config.json to lift the cap.",
                               round, max_rounds)
            : wxString::Format("Agent is on tool-call round %d (cap disabled).",
                               round));
    const bool was_hidden = !round_chip_->IsShown();
    round_chip_->Show();
    return was_hidden;  // re-layout only when first surfaced
}

bool ChatFooterChips::hide_round_progress()
{
    if (!round_chip_) return false;
    if (!round_chip_->IsShown()) return false;
    round_chip_->SetLabel("");
    round_chip_->Hide();
    return true;
}

bool ChatFooterChips::set_compacted_count(int count, const wxString& archive_dir)
{
    if (!compacted_btn_) return false;
    compacted_archive_dir_ = archive_dir;
    if (count <= 0) {
        if (!compacted_btn_->IsShown()) return false;
        compacted_btn_->SetLabel("0");
        compacted_btn_->Hide();
        return true;
    }
    const bool was_hidden = !compacted_btn_->IsShown();
    compacted_btn_->SetLabel(wxString::Format("%d", count));
    wxString tip = wxString::Format(
        "%d compaction%s in this session", count, count == 1 ? "" : "s");
    if (!archive_dir.empty())
        tip += " - click to open archive folder";
    compacted_btn_->SetToolTip(tip);
    compacted_btn_->Show();
    return was_hidden;  // re-layout only on visibility flip
}

bool ChatFooterChips::on_plan_proposed(const std::string& plan_id,
                                        int total_steps,
                                        const std::string& first_step_desc)
{
    current_plan_id_          = plan_id;
    current_plan_total_steps_ = total_steps;
    current_plan_done_steps_  = 0;
    current_plan_step_label_  = first_step_desc;
    plan_done_                = false;
    plan_done_success_        = true;
    return true;  // text changed -- status-bar should refresh
}

bool ChatFooterChips::on_plan_step_advanced(const std::string& plan_id,
                                             const wxString& status)
{
    if (plan_id != current_plan_id_) return false;
    if (status != "done" && status != "failed") return false;
    ++current_plan_done_steps_;
    return true;
}

bool ChatFooterChips::on_plan_completed(const std::string& plan_id, bool success)
{
    if (plan_id != current_plan_id_) return false;
    plan_done_         = true;
    plan_done_success_ = success;
    return true;
}

bool ChatFooterChips::hide_plan_chip()
{
    const bool had_plan = !current_plan_id_.empty();
    current_plan_id_.clear();
    current_plan_total_steps_ = 0;
    current_plan_done_steps_  = 0;
    current_plan_step_label_.clear();
    plan_done_         = false;
    plan_done_success_ = true;
    return had_plan;
}

wxString ChatFooterChips::plan_text() const
{
    if (current_plan_id_.empty()) return {};
    if (plan_done_) {
        return wxString::Format("Plan: done (%d/%d%s)",
                                current_plan_done_steps_,
                                current_plan_total_steps_,
                                plan_done_success_ ? "" : " - with failures");
    }
    return format_plan_chip(current_plan_done_steps_,
                            current_plan_total_steps_,
                            current_plan_step_label_);
}

wxString ChatFooterChips::commit_text() const
{
    if (commit_short_sha_.empty()) return {};
    wxString label = "Commit: " + commit_short_sha_;
    if (!commit_branch_.empty())
        label += " (" + commit_branch_ + ")";
    return label;
}

void ChatFooterChips::refresh_ctx_label()
{
    int used    = last_ctx_used_;
    int limit   = last_ctx_limit_;
    int reserve = last_ctx_reserve_;
    // Effective limit is the user-visible budget: raw window minus the
    // reserved headroom the agent loop keeps free for the next completion.
    int eff_limit = (reserve > 0 && limit > reserve) ? (limit - reserve) : limit;
    int pct = (eff_limit > 0) ? (used * 100 / eff_limit) : 0;

    wxString label = wxString::Format("ctx: %d/%d (%d%%)", used, limit, pct);
    if (ctx_label_) ctx_label_->SetLabel(label);

    if (ctx_label_) {
        // Detailed breakdown moves into the tooltip so the visible label
        // stays short. Hover reveals total / prompt / last-or-live
        // completion / reserved headroom.
        wxString tip = wxString::Format("Total: %d / %d tokens", used, limit);
        if (last_ctx_prompt_ > 0)
            tip += wxString::Format("\n  Prompt: %d", last_ctx_prompt_);
        if (last_ctx_completion_ > 0)
            tip += wxString::Format("\n  Last completion: %d", last_ctx_completion_);
        else if (live_completion_estimate_ > 0)
            tip += wxString::Format("\n  Live completion estimate: ~%d",
                                    live_completion_estimate_);
        if (reserve > 0)
            tip += wxString::Format("\n  Reserved headroom: %d (effective limit: %d)",
                                    reserve, eff_limit);
        ctx_label_->SetToolTip(tip);
    }
}

} // namespace locus
