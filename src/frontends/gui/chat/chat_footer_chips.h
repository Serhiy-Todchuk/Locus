#pragma once

#include <string>

#include <wx/wx.h>

namespace locus {

// Owns the chat footer's lightweight chip widgets. After the footer
// declutter pass, only three things still live IN the footer:
//
//   ctx_label       : "ctx: 123456/200000 (62%)" (in/out split in tooltip)
//   round_chip      : "round 7/500" while a turn is in flight
//   compacted_btn   : a small numeric button (e.g. "12") that opens the
//                     session's history-archive folder on click
//
// Plan progress and last auto-commit moved to the main-window status bar
// (their underlying state is still tracked here so the status bar can read
// `plan_text()` / `commit_text()` and re-render on tab switch). The chat-
// side widgets for those two chips were retired.
class ChatFooterChips {
public:
    // All widgets are parented to `parent` (the ChatPanel itself).
    explicit ChatFooterChips(wxWindow* parent);

    // Widget accessors for ChatPanel sizer assembly. The compacted button
    // is a button now (was a wxStaticText) so it gets standard hover +
    // click feel.
    wxStaticText* ctx_label()       const { return ctx_label_; }
    wxButton*     compacted_btn()   const { return compacted_btn_; }
    wxStaticText* round_chip()      const { return round_chip_; }

    // Context meter. Called from ChatPanel::set_context_meter. The visible
    // label only carries "ctx: <used>/<limit> (P%)"; the prompt /
    // completion / reserve breakdown is in the tooltip.
    // Returns true when a Layout() call is needed (label width changed).
    bool set_context_meter(int used, int limit,
                           int prompt_tokens, int completion_tokens,
                           int reserve_tokens = 0);

    // Live generation estimate update -- consumed by the tooltip only.
    void set_generation_progress(int chars, int est_tokens);

    // Clear the live estimate after turn complete so the exact server value wins.
    void clear_live_estimate();

    // Auto-commit cache update. Stores the latest commit; the rendered
    // string is exposed via commit_text() so LocusFrame can write it to
    // the status bar. Returns true so callers know commit_text() changed
    // and the active tab may need to push a status-bar refresh.
    bool on_auto_commit(const wxString& short_sha,
                        const wxString& branch,
                        const wxString& subject);

    // S5.Z task 6 -- compactions counter button. `count <= 0` hides it;
    // positive values render the number itself as the label.
    // S6.18 C.3 -- when `no_op_count > 0` the label suffixes "(M no-op)"
    // and the chip widens to fit; the tooltip explains the no-op concept.
    // `archive_dir` is the user-facing path that opens on click; pass
    // empty to disable the click handler. Returns true if Layout() is
    // needed (visibility flip or label width change).
    bool set_compacted_count(int count, int no_op_count,
                             const wxString& archive_dir);

    // Agentic Tetris findings #5 -- in-flight round counter.
    bool set_round_progress(int round, int max_rounds);
    bool hide_round_progress();

    // S6.20 -- transient LLM-transport status (retry/backoff). Reuses the
    // round_chip widget area: shows e.g. "HTTP 429 -- retrying 2/5 in 8s" so a
    // multi-minute backoff isn't a silent hang. Superseded by the next
    // set_round_progress / hide_round_progress (which fire on the next round /
    // turn complete). Returns true when Layout() is needed.
    bool set_transient_status(const wxString& status);

    // S6.18 C.4 -- per-session auto-corrections counter chip. The chip is
    // hidden when N == 0; reads "auto-corrections: N" when N > 0. Source
    // of truth is the `quality_correction` activity-log event count; this
    // accessor lets ChatPanel bump on each event without owning the
    // widget itself. Returns true when Layout is needed.
    bool set_auto_correction_count(int count);
    wxStaticText* auto_corrections_chip() const { return auto_corrections_chip_; }

    // Plan-chip state updates (no chat-side widget after the declutter
    // pass; LocusFrame reads `plan_text()` and writes it to a status-bar
    // field). Return value flags "the rendered string changed; refresh
    // anyone listening".
    bool on_plan_proposed(const std::string& plan_id, int total_steps,
                          const std::string& first_step_desc);
    bool on_plan_step_advanced(const std::string& plan_id, const wxString& status);
    bool on_plan_completed(const std::string& plan_id, bool success);

    // Drop plan state (e.g. on_mode_changed to chat, or session reset).
    // Returns true if the rendered text changed (so the status-bar field
    // should be cleared by the caller).
    bool hide_plan_chip();

    // Active-plan id is still kept here so ChatPanel can hand it to
    // ChatLinkHandler::handle_url.
    const std::string& current_plan_id() const { return current_plan_id_; }

    // Rendered text accessors for LocusFrame's status bar -- empty string
    // means "no info; clear the field".
    wxString plan_text()   const;
    wxString commit_text() const;

    // Click target for the compacted button. ChatPanel binds the handler
    // (it knows the archive_dir to launch).
    const wxString& compacted_archive_dir() const { return compacted_archive_dir_; }

private:
    void refresh_ctx_label();

    wxStaticText* ctx_label_      = nullptr;
    wxButton*     compacted_btn_  = nullptr;
    wxStaticText* round_chip_     = nullptr;
    wxStaticText* auto_corrections_chip_ = nullptr;  // S6.18 C.4

    int last_ctx_used_            = 0;
    int last_ctx_limit_           = 0;
    int last_ctx_prompt_          = 0;
    int last_ctx_completion_      = 0;
    int last_ctx_reserve_         = 0;
    int live_completion_estimate_ = 0;

    // Plan state -- no widget; status bar reads `plan_text()`.
    std::string current_plan_id_;
    int         current_plan_total_steps_ = 0;
    int         current_plan_done_steps_  = 0;
    std::string current_plan_step_label_;
    bool        plan_done_           = false;
    bool        plan_done_success_   = true;

    // Auto-commit state -- no widget; status bar reads `commit_text()`.
    wxString    commit_short_sha_;
    wxString    commit_branch_;
    wxString    commit_subject_;

    // Path the compacted button opens on click; surfaced via accessor.
    wxString    compacted_archive_dir_;
};

} // namespace locus
