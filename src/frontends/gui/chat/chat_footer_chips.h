#pragma once

#include <string>

#include <wx/gauge.h>
#include <wx/wx.h>

namespace locus {

// Owns the four footer chips in ChatPanel's bottom bar:
//   ctx_gauge / ctx_label : context meter (used/limit/prompt/completion split)
//   plan_chip             : current plan progress ("Plan: 3/7 - building ...")
//   commit_chip           : last auto-commit short SHA + branch
//
// All widgets are parented to the ChatPanel. Getters expose them for sizer
// assembly in ChatPanel::create_footer. Internally tracks plan progress
// (which plan is active, step counts) to keep chip text up to date.
class ChatFooterChips {
public:
    // All widgets are parented to `parent` (the ChatPanel itself).
    explicit ChatFooterChips(wxWindow* parent);

    // Widget accessors for ChatPanel sizer assembly.
    wxGauge*      gauge()       const { return ctx_gauge_; }
    wxStaticText* ctx_label()   const { return ctx_label_; }
    wxStaticText* plan_chip()   const { return plan_chip_; }
    wxStaticText* commit_chip() const { return commit_chip_; }

    // Context meter. Called from ChatPanel::set_context_meter.
    // Returns true when a Layout() call is needed (label width may have changed).
    // reserve_tokens (S5.D) is the headroom the agent loop keeps free; gauge
    // color thresholds are applied against effective_limit = limit - reserve.
    bool set_context_meter(int used, int limit,
                           int prompt_tokens, int completion_tokens,
                           int reserve_tokens = 0);

    // Live generation estimate update. Called from set_generation_progress.
    void set_generation_progress(int chars, int est_tokens);

    // Clear the live estimate after turn complete so the exact server value wins.
    void clear_live_estimate();

    // Auto-commit chip. Returns true if Layout() is needed.
    bool on_auto_commit(const wxString& short_sha,
                        const wxString& branch,
                        const wxString& subject);

    // Plan chip updates. Each returns true if Layout() is needed.
    bool on_plan_proposed(const std::string& plan_id, int total_steps,
                          const std::string& first_step_desc);
    bool on_plan_step_advanced(const std::string& plan_id, const wxString& status);
    bool on_plan_completed(const std::string& plan_id, bool success);

    // Hide the plan chip and clear plan state (e.g. on_mode_changed to chat,
    // or session reset). Returns true if Layout() is needed.
    bool hide_plan_chip();

    // Getter used by ChatPanel to pass to ChatLinkHandler::handle_url.
    const std::string& current_plan_id() const { return current_plan_id_; }

private:
    void refresh_ctx_label();

    wxGauge*      ctx_gauge_  = nullptr;
    wxStaticText* ctx_label_  = nullptr;
    wxStaticText* plan_chip_  = nullptr;
    wxStaticText* commit_chip_ = nullptr;

    int last_ctx_used_            = 0;
    int last_ctx_limit_           = 0;
    int last_ctx_prompt_          = 0;
    int last_ctx_completion_      = 0;
    int last_ctx_reserve_         = 0;
    int live_completion_estimate_ = 0;

    std::string current_plan_id_;
    int         current_plan_total_steps_ = 0;
    int         current_plan_done_steps_  = 0;
    std::string current_plan_step_label_;
};

} // namespace locus
