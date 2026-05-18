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
    ctx_gauge_ = new wxGauge(parent, wxID_ANY, 100,
                             wxDefaultPosition, wxSize(120, 16));
    ctx_label_ = new wxStaticText(parent, wxID_ANY, "ctx: 0/0",
                                  wxDefaultPosition, wxSize(150, -1));
    plan_chip_      = new wxStaticText(parent, wxID_ANY, "");
    commit_chip_    = new wxStaticText(parent, wxID_ANY, "");
    compacted_chip_ = new wxStaticText(parent, wxID_ANY, "");
    plan_chip_->Hide();
    commit_chip_->Hide();
    compacted_chip_->Hide();

    ctx_label_->SetName(ui_names::kChatCtxLabel);
    plan_chip_->SetName(ui_names::kChatPlanChip);
    commit_chip_->SetName(ui_names::kChatCommitChip);
    compacted_chip_->SetName(ui_names::kChatCompactedChip);
    gui::apply_locus_accessible_name(ctx_label_);
    gui::apply_locus_accessible_name(plan_chip_);
    gui::apply_locus_accessible_name(commit_chip_);
    gui::apply_locus_accessible_name(compacted_chip_);
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
    return false;  // label width is fixed; no re-layout needed
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
    if (!commit_chip_ || short_sha.empty()) return false;

    wxString label = "Commit: " + short_sha;
    if (!branch.empty())
        label += " (" + branch + ")";
    commit_chip_->SetLabel(label);

    wxString tip = subject;
    if (!branch.empty())
        tip = branch + " " + short_sha + ": " + subject;
    commit_chip_->SetToolTip(tip);

    const bool was_hidden = !commit_chip_->IsShown();
    commit_chip_->Show();
    return was_hidden;  // layout needed only when just shown
}

bool ChatFooterChips::set_compacted_count(int count, const wxString& archive_dir)
{
    if (!compacted_chip_) return false;
    if (count <= 0) {
        if (!compacted_chip_->IsShown()) return false;
        compacted_chip_->SetLabel("");
        compacted_chip_->Hide();
        return true;
    }
    const bool was_hidden = !compacted_chip_->IsShown();
    compacted_chip_->SetLabel(wxString::Format("compacted: %d", count));
    wxString tip = wxString::Format(
        "%d compaction%s in this session", count, count == 1 ? "" : "s");
    if (!archive_dir.empty())
        tip += " - click to open archive folder";
    compacted_chip_->SetToolTip(tip);
    compacted_chip_->Show();
    return was_hidden;  // re-layout only when first surfaced
}

bool ChatFooterChips::on_plan_proposed(const std::string& plan_id,
                                        int total_steps,
                                        const std::string& first_step_desc)
{
    current_plan_id_          = plan_id;
    current_plan_total_steps_ = total_steps;
    current_plan_done_steps_  = 0;
    current_plan_step_label_  = first_step_desc;

    if (!plan_chip_) return false;
    plan_chip_->SetLabel(format_plan_chip(0, total_steps, first_step_desc));
    const bool was_hidden = !plan_chip_->IsShown();
    plan_chip_->Show();
    return true;  // always re-layout (label text changed)
}

bool ChatFooterChips::on_plan_step_advanced(const std::string& plan_id,
                                             const wxString& status)
{
    if (plan_id != current_plan_id_) return false;
    if (status != "done" && status != "failed") return false;

    ++current_plan_done_steps_;
    if (!plan_chip_) return false;
    plan_chip_->SetLabel(
        format_plan_chip(current_plan_done_steps_,
                         current_plan_total_steps_,
                         current_plan_step_label_));
    return true;
}

bool ChatFooterChips::on_plan_completed(const std::string& plan_id, bool success)
{
    if (plan_id != current_plan_id_) return false;
    if (!plan_chip_) return false;
    plan_chip_->SetLabel(
        wxString::Format("Plan: done (%d/%d%s)",
                         current_plan_done_steps_,
                         current_plan_total_steps_,
                         success ? "" : " - with failures"));
    return true;
}

bool ChatFooterChips::hide_plan_chip()
{
    current_plan_id_.clear();
    current_plan_total_steps_ = 0;
    current_plan_done_steps_  = 0;
    current_plan_step_label_.clear();
    if (!plan_chip_ || !plan_chip_->IsShown()) return false;
    plan_chip_->SetLabel("");
    plan_chip_->Hide();
    return true;
}

void ChatFooterChips::refresh_ctx_label()
{
    int used    = last_ctx_used_;
    int limit   = last_ctx_limit_;
    int reserve = last_ctx_reserve_;
    // S5.D -- gauge and color thresholds are against the effective limit
    // (raw window minus the reserved headroom) so the gauge turns red when
    // the user-visible budget is exhausted, not when the LLM's output space
    // is consumed.
    int eff_limit = (reserve > 0 && limit > reserve) ? (limit - reserve) : limit;
    int pct = (eff_limit > 0) ? (used * 100 / eff_limit) : 0;
    if (ctx_gauge_) ctx_gauge_->SetValue(std::min(pct, 100));

    wxString out_part;
    if (last_ctx_completion_ > 0) {
        out_part = wxString::Format(" out:%d", last_ctx_completion_);
    } else if (live_completion_estimate_ > 0) {
        out_part = wxString::Format(" out:~%d", live_completion_estimate_);
    }
    wxString in_part;
    if (last_ctx_prompt_ > 0) {
        in_part = wxString::Format(" in:%d", last_ctx_prompt_);
    }

    wxString label;
    if (!in_part.IsEmpty() || !out_part.IsEmpty()) {
        label = wxString::Format("ctx: %d/%d (%d%%,%s%s)",
                                 used, limit, pct,
                                 in_part.c_str(), out_part.c_str());
    } else {
        label = wxString::Format("ctx: %d/%d (%d%%)", used, limit, pct);
    }
    if (ctx_label_) ctx_label_->SetLabel(label);

    // Tooltip: stacked breakdown so the user understands the split.
    if (ctx_label_) {
        wxString tip = wxString::Format("Total: %d / %d tokens", used, limit);
        if (last_ctx_prompt_ > 0)
            tip += wxString::Format("\n  Prompt: %d", last_ctx_prompt_);
        if (last_ctx_completion_ > 0)
            tip += wxString::Format("\n  Last completion: %d", last_ctx_completion_);
        if (reserve > 0)
            tip += wxString::Format("\n  Reserved headroom: %d (effective limit: %d)",
                                    reserve, eff_limit);
        ctx_label_->SetToolTip(tip);
    }

    if (ctx_gauge_) {
        if (pct < 60)
            ctx_gauge_->SetForegroundColour(wxColour(76, 175, 80));
        else if (pct < 80)
            ctx_gauge_->SetForegroundColour(wxColour(255, 193, 7));
        else
            ctx_gauge_->SetForegroundColour(wxColour(244, 67, 54));
    }
}

} // namespace locus
