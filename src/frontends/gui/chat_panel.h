#pragma once

#include "../../agent/agent_mode.h"
#include "../../agent/system_prompt_assembly.h"
#include "../../core/frontend.h"
#include "../../llm/llm_client.h"   // MessageRole
#include "../../tools/permission_presets.h"
#include "slash_popup.h"

#include <wx/wx.h>
#include <wx/tglbtn.h>
#include <wx/webview.h>

#include <nlohmann/json.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace locus {

class ChatFooterChips;
class ChatLinkHandler;
class ChatPopups;
class ChatStreamRenderer;

// Chat display + input panel. Center pane of the main frame.
//
// Layout (vertical):
//   [wxWebView   -- chat history, HTML/CSS rendered                      ] (stretches)
//   [wxTextCtrl  -- multiline input, Enter=send                          ] (fixed height)
//   [footer bar  -- context gauge + plan/commit/preset chips + buttons   ] (fixed height)
//
// Streaming: tokens are buffered and flushed to the WebView via a wxTimer
// every 33ms (~30fps). md4c converts accumulated markdown to HTML on each
// flush. Prism.js re-highlights code blocks on turn complete.
//
// Collaborators (each owns a coherent slice of state):
//   ChatStreamRenderer -- token buffer, assistant/reasoning bubble state
//   ChatFooterChips    -- context gauge, plan chip, commit chip
//   ChatPopups         -- slash-command and @-mention autocomplete popups
//   ChatLinkHandler    -- locus:// URL dispatch + plan_msg_ids mapping

class ChatPanel : public wxPanel {
public:
    // on_send is called with the user's message text when Enter is pressed.
    // on_compact is called when the user clicks the manual compaction button.
    // on_stop is called when the user clicks Stop during generation.
    // on_undo is called when the user clicks the Undo button.
    // S4.D plan-mode callbacks. on_mode_pick invoked on Chat/Plan/Execute click.
    // on_plan_decision invoked when Approve or Reject clicked in plan bubble.
    ChatPanel(wxWindow* parent,
              std::function<void(const std::string&)> on_send,
              std::function<void()> on_compact = nullptr,
              std::function<void()> on_stop = nullptr,
              std::function<void()> on_undo = nullptr,
              std::function<void(AgentMode)> on_mode_pick = nullptr,
              std::function<void(const std::string&)> on_plan_decision = nullptr);

    ~ChatPanel();

    // -- Called by LocusFrame in response to agent events --

    void on_turn_start();
    void on_token(const wxString& token);
    void on_reasoning_token(const wxString& token);
    void on_turn_complete();
    void on_session_reset();
    void on_error(const wxString& message);

    // Tool call / result display (shown inline in chat). The call_id is
    // remembered so on_tool_result can attach to the correct DOM node.
    //
    // S5.C -- `args` is the raw tool-call args JSON cached against the call_id
    // so on_tool_result can render an inline diff for successful
    // edit_file / write_file / delete_file calls.
    void on_tool_pending(const wxString& call_id,
                         const wxString& tool_name,
                         const wxString& preview,
                         const nlohmann::json& args);
    void on_tool_result(const wxString& call_id,
                        const wxString& display,
                        bool success);

    // S5.C -- supplied by LocusFrame. Returns the pre-mutation snapshot for
    // `rel_path` from the current turn's checkpoint, or nullopt when none exists.
    using FetchPreMutationFn =
        std::function<std::optional<std::string>(const std::string& rel_path)>;
    void set_pre_mutation_fetcher(FetchPreMutationFn fn) {
        pre_mutation_fetcher_ = std::move(fn);
    }

    void set_diff_options(bool show_diffs, int max_lines,
                          int context_lines = 4,
                          int collapse_threshold = 16) {
        diff_show_               = show_diffs;
        diff_max_lines_          = max_lines > 0 ? max_lines : 200;
        diff_context_lines_      = context_lines >= 0 ? context_lines : 4;
        diff_collapse_threshold_ = collapse_threshold >= 0 ? collapse_threshold : 16;
    }

    // S4.D plan-mode display.
    void on_mode_changed(AgentMode mode);
    void on_plan_proposed(const wxString& plan_json);
    void on_plan_step_advanced(const wxString& plan_id, int step_idx,
                                const wxString& status,
                                const wxString& notes);
    void on_plan_completed(const wxString& plan_id, bool success);

    // S4.L auto-commit chip.
    void on_auto_commit(const wxString& short_sha,
                        const wxString& branch,
                        const wxString& subject);

    // Footer-declutter pass: plan + commit chips moved out of the chat
    // footer into LocusFrame's status bar. The status bar is global (one
    // per main window), so it reads the active tab's cached strings via
    // these accessors -- both on plan/commit events and on tab switch.
    // Empty string means "no info; clear the status bar field".
    wxString plan_status_text()   const;
    wxString commit_status_text() const;

    // S5.Z task 6 -- compactions counter chip. `archive_dir` is opened with
    // the OS default app on chip click. Pass count == 0 to hide.
    // S6.18 C.3 -- `no_op_count` is the running total of reached=no
    // compactions in this session; when > 0 the chip text reads
    // "compacted: N (M no-op)" so a stuck pipeline is visible at a glance.
    void set_compacted_count(int count, int no_op_count,
                             const wxString& archive_dir);

    // S6.18 C.4 -- session-scoped auto-corrections counter. AgentEventRouter
    // calls bump() once per ActivityKind::quality_correction event from the
    // source tab; reset_auto_correction_count() runs on session reset so a
    // new conversation starts at zero.
    void bump_auto_correction_count();
    void reset_auto_correction_count();

    // Agentic Tetris findings #5 -- in-flight round counter ("round N/M").
    // Pass round <= 0 to hide. `max_rounds == 0` means "unbounded" and
    // renders just "round N".
    void set_round_progress(int round, int max_rounds);

    // S6.20 -- transient LLM-transport retry/backoff notice in the footer
    // status area. Superseded by the next round-progress / turn-complete event.
    void set_transient_status(const wxString& status);

    // S6.13 follow-up -- show / hide the Commit-now button. Called from
    // WxFrontend::on_reasoning_watchdog_tripped (show) and
    // on_reasoning_watchdog_cleared / on_turn_complete (hide). Idempotent.
    void set_commit_now_visible(bool visible);

    // Set the callback invoked when the user clicks Commit-now. LocusFrame
    // points this at the active tab's `ILocusCore::request_commit_now()`.
    void set_on_commit_now(std::function<void()> cb) {
        on_commit_now_ = std::move(cb);
    }

    // Footer updates. `stream_ms_last_round` is the wall-clock duration of
    // the most recent LLM stream call; ChatPanel pairs it with completion
    // tokens to render a tok/s rate next to the bubble's token chip.
    void set_context_meter(int used, int limit,
                           int prompt_tokens = 0, int completion_tokens = 0,
                           int reserve_tokens = 0,
                           long long stream_ms_last_round = 0);
    // S5.D -- show/hide per-message token chips.
    void set_show_per_message_tokens(bool show);
    void set_generation_progress(int chars, int est_tokens);

    // Auto-compact toggle in the chat footer. LocusFrame mirrors the
    // workspace config's compaction.auto_enabled value here and listens
    // for user toggles via set_on_auto_compact_toggle().
    void set_auto_compact_state(bool checked);
    void set_on_auto_compact_toggle(std::function<void(bool)> cb) {
        on_auto_compact_toggle_ = std::move(cb);
    }

    // S5.S -- permission preset chip + combobox in the chat footer.
    // `effective` is the preset to render; `is_runtime_override` controls
    // the chip border style. Called by LocusFrame in response to
    // IFrontend::on_permission_preset_changed.
    void on_permission_preset_changed(tools::PermissionPreset effective,
                                       bool is_runtime_override);
    // Callback fired when the user picks a new preset from the chat-footer
    // dropdown. "custom" is never emitted -- selecting Custom from the menu
    // is a no-op at the panel layer (no signature to apply). Passing a value
    // means: apply this as a runtime override. nullopt means: clear the
    // runtime override and revert to the saved setting.
    using PermissionPresetPickFn =
        std::function<void(std::optional<tools::PermissionPreset>)>;
    void set_on_permission_preset_pick(PermissionPresetPickFn cb) {
        on_permission_preset_pick_ = std::move(cb);
    }

    // S6.16 -- active LLM endpoint picker in the chat footer. `set_endpoints`
    // populates the choice from the EndpointProfileStore (one row per profile
    // name) + a trailing "Edit endpoints..." sentinel; `active` selects the
    // current row. `set_active_endpoint` is the no-op-if-same updater driven by
    // the on_endpoint_changed event. `set_endpoint_tooltip` shows base_url +
    // masked key. on_endpoint_pick fires with a profile name when the user
    // picks a normal row; on_open_endpoint_settings fires when they pick the
    // sentinel (the selection reverts to the active row).
    void set_endpoints(const std::vector<std::string>& names,
                       const std::string& active);
    void set_active_endpoint(const std::string& name);
    void set_endpoint_tooltip(const wxString& tip);
    void set_on_endpoint_pick(std::function<void(const std::string&)> cb) {
        on_endpoint_pick_ = std::move(cb);
    }
    void set_on_open_endpoint_settings(std::function<void()> cb) {
        on_open_endpoint_settings_ = std::move(cb);
    }

    // S5.G -- collapsed system-prompt bubble at the top of the chat. Renders
    // the full prompt text + per-section breakdown chips. Owned by AgentCore;
    // the chat panel just displays. Call once at construction (and on session
    // reset). The bubble lives at dom_id=0 (a reserved slot outside the
    // ChatPanel's monotonic message_id_ allocator).
    void set_system_prompt_bubble(const SystemPromptAssembly& assembly);

    // S5.G -- a ChatMessage was just appended to ConversationHistory. Used to
    // map the most recent dom bubble of `role` to `history_id` and (when
    // deletable) inject the hover-reveal X.
    void on_history_message_added(int history_id, MessageRole role,
                                   bool deletable);

    // S5.I follow-up -- render a previously-saved conversation into the chat
    // panel.  Called once when a tab restores from disk (the live agent path
    // already paints bubbles for new messages; this only fires for content
    // that came from the session JSON).  Walks the history in order, emits
    // reasoning / user / assistant / tool bubbles matching the live look,
    // attaches the hover-reveal X to deletable ones, and populates the
    // history_to_dom_ map so per-message delete works on restored content.
    //
    // `tools` is optional -- when supplied, each tool-result bubble shows the
    // same `tool-preview` line the live `on_tool_pending` path renders (built
    // by re-invoking ITool::preview on the saved tool_calls arguments). When
    // null, only the tool name is shown above the result body.
    void render_loaded_history(const class ConversationHistory& history,
                               class IToolRegistry* tools = nullptr);

    // S5.G -- a ChatMessage was removed from ConversationHistory. Removes the
    // matching dom bubble.
    void on_history_message_deleted(int history_id);

    // S5.G -- frame supplies the confirm-then-delete dispatch closure. Called
    // by ChatLinkHandler when the user clicks the hover-reveal X.
    using DeleteFn = std::function<void(int history_id)>;
    void set_on_delete_message(DeleteFn fn) { on_delete_message_ = std::move(fn); }

    // Attached-context chip (above input).
    void set_attached_chip(const wxString& file_path);
    void set_on_detach(std::function<void()> cb);

    // Slash-command suggestions.
    void set_slash_commands(std::vector<SlashItem> items);

    // S4.V @-mention support.
    void set_mention_paths(std::vector<std::string> paths);
    void set_on_mention_attach(std::function<void(const std::string&)> cb);

    // GUI slash command dispatch (see original header for full contract).
    void set_on_slash_command(std::function<bool(const std::string&,
                                                 const std::string&)> cb);

    // Append a system-style note into the chat (used by /help).
    void append_system_note(const wxString& html);

    // S5.Z task 2 -- in-conversation find bar.  Toggled by the footer
    // magnifier button + the View > Find in Conversation menu item (Ctrl+F).
    // The menu accelerator is the only path that survives Win32 native
    // controls (chat input is RichEdit, which swallows key messages before
    // they propagate up the wx parent chain).  LocusFrame's MenuController
    // hook resolves the active chat tab and calls toggle_find_bar.  Pure
    // frontend; no agent-thread plumbing.
    void toggle_find_bar();
    bool is_find_bar_visible() const;

private:
    void create_webview();
    void create_input();
    void create_footer();
    void create_find_bar();

    // S5.Z task 2 helpers.
    void show_find_bar();
    void hide_find_bar();
    void find_apply();           // re-issues Find for current query, resets counter.
    void find_step(bool forward);// Next / Prev with wrap.
    void update_find_counter();
    int  current_find_flags() const;
    void on_find_input_key(wxKeyEvent& evt);

    void run_script(const wxString& js);

    // Flip page_ready_ and flush any scripts queued before the WebView's
    // inline document script finished defining its functions. Idempotent --
    // both the wxEVT_WEBVIEW_LOADED handler (fires after inline script on
    // WebView2) and the page-emitted `locus://ready` navigation (the reliable
    // signal on WKWebView, where LOADED fires *before* the inline script) call
    // it; whichever lands first wins and the other is a no-op.
    void mark_page_ready();

    static wxString user_text_to_html(const wxString& s);

    void on_flush_timer(wxTimerEvent& evt);
    void on_input_key(wxKeyEvent& evt);
    void on_input_text(wxCommandEvent& evt);
    void on_webview_navigating(wxWebViewEvent& evt);

    bool submit_current_input();
    void refresh_action_btn();

    // Must precede the collaborator unique_ptrs: the renderer holds `int&` into this.
    int message_id_ = 0;

    // S6.18 C.4 -- session-scoped auto-corrections counter. Incremented by
    // AgentEventRouter on every ActivityKind::quality_correction event from
    // this tab; reset on session reset.
    int auto_corrections_count_ = 0;

    // Collaborators (owned by unique_ptr so forward declarations above suffice).
    std::unique_ptr<ChatStreamRenderer> renderer_;
    std::unique_ptr<ChatFooterChips>    footer_chips_;
    std::unique_ptr<ChatPopups>         popups_;
    std::unique_ptr<ChatLinkHandler>    link_handler_;

    // Callbacks from LocusFrame.
    std::function<void(const std::string&)> on_send_;
    std::function<void()>                    on_compact_;
    std::function<void(bool)>                on_auto_compact_toggle_;
    std::function<void()>                    on_stop_;
    std::function<void()>                    on_undo_;
    // S6.13 follow-up -- invoked when the user clicks Commit-now. LocusFrame
    // wires to the active tab's request_commit_now(). Default empty -> click
    // is a silent no-op (the button only surfaces when wired anyway).
    std::function<void()>                    on_commit_now_;
    std::function<void(AgentMode)>           on_mode_pick_;
    std::function<void(const std::string&)>  on_plan_decision_;
    std::function<bool(const std::string&, const std::string&)> on_slash_command_;
    std::function<void(const std::string&)>  on_mention_attach_;

    // Core widgets.
    wxWebView*    webview_       = nullptr;
    wxTextCtrl*   input_         = nullptr;
    wxButton*     compact_btn_   = nullptr;
    wxCheckBox*   auto_compact_cb_ = nullptr;
    wxButton*     stop_btn_      = nullptr;
    wxButton*     undo_btn_      = nullptr;
    // S6.13 follow-up -- Commit-now button. Hidden by default; visibility
    // toggled by set_commit_now_visible() in response to reasoning-watchdog
    // events from the agent thread. Click invokes on_commit_now_ which
    // LocusFrame wires to the active tab's `ILocusCore::request_commit_now()`.
    wxButton*     commit_btn_    = nullptr;
    wxToggleButton* mode_chat_btn_    = nullptr;
    wxToggleButton* mode_plan_btn_    = nullptr;
    wxToggleButton* mode_execute_btn_ = nullptr;

    // S5.S permission preset dropdown. The constant "Permission:" prefix
    // label was retired in the footer declutter pass; combo carries its
    // own tooltip + colour cue.
    wxChoice*              preset_choice_      = nullptr;
    tools::PermissionPreset preset_effective_   = tools::PermissionPreset::ask_before_edits;
    bool                   preset_is_runtime_  = false;
    PermissionPresetPickFn on_permission_preset_pick_;

    // S6.16 -- endpoint picker. endpoint_names_ mirrors the non-sentinel rows
    // so the selection handler can map index -> name and snap back to the
    // active row when the sentinel is chosen.
    wxChoice*                                endpoint_choice_ = nullptr;
    std::vector<std::string>                 endpoint_names_;
    std::string                              endpoint_active_;
    std::function<void(const std::string&)>  on_endpoint_pick_;
    std::function<void()>                    on_open_endpoint_settings_;

    // S5.Z task 2 find-in-chat bar.  Hidden by default; toggled via the
    // View > Find in Conversation menu item (Ctrl+F) or the footer Find
    // button.  Lives at the top of the chat panel's vertical sizer so it
    // pushes chat content down rather than overlaying it (overlay would need
    // a wxPopupTransientWindow which doesn't play well with WebView2's
    // focus model).
    wxPanel*      find_bar_         = nullptr;
    wxTextCtrl*   find_input_       = nullptr;
    wxStaticText* find_counter_     = nullptr;
    wxButton*     find_prev_btn_    = nullptr;
    wxButton*     find_next_btn_    = nullptr;
    wxCheckBox*   find_case_toggle_ = nullptr;
    wxButton*     find_close_btn_   = nullptr;
    wxButton*     find_btn_         = nullptr; // footer magnifier toggle
    long          find_total_       = 0;       // wxWebView::Find return for current query
    long          find_index_       = 0;       // 1-based current match (0 when none)
    wxString      find_active_query_;          // last non-empty query handed to Find()

    // (S5.Z task 6 archive-dir state moved into ChatFooterChips alongside
    // the compacted_btn_ click handler.)

    // Attached-context chip row.
    wxPanel*      attach_panel_  = nullptr;
    wxStaticText* attach_label_  = nullptr;
    wxButton*     attach_close_  = nullptr;
    std::function<void()> on_detach_;

    wxTimer flush_timer_;

    // call_id -> message_id mapping for tool-pending / tool-result pairing.
    std::unordered_map<std::string, int> tool_call_msg_ids_;

    // S5.C -- per call_id cache so on_tool_result can render inline diffs.
    struct PendingToolInfo {
        std::string    tool_name;
        nlohmann::json args;
    };
    std::unordered_map<std::string, PendingToolInfo> pending_tool_info_;

    FetchPreMutationFn pre_mutation_fetcher_;
    bool               diff_show_               = true;
    int                diff_max_lines_          = 200;
    int                diff_context_lines_      = 4;
    int                diff_collapse_threshold_ = 16;

    // S5.D -- per-message token chip state.
    bool show_per_message_tokens_   = true;
    int  last_completion_tokens_    = 0;
    // Wall-clock of the most recent LLM stream round, paired with
    // last_completion_tokens_ to compute tok/s on the assistant bubble.
    long long last_stream_ms_       = 0;

    // S5.G -- chat-side bookkeeping for per-message delete.
    // FIFO queue of user-bubble dom_ids whose history_id is still pending
    // (one entry pushed per submit, popped per role=user
    // on_history_message_added). A deque (not a single int) is required for
    // mid-turn injection (`AgentCore` can pair multiple queued user messages
    // back-to-back against multiple in-flight bubbles).
    std::deque<int> pending_user_dom_ids_;
    // Set by on_history_message_added(role=tool); consumed by on_tool_result
    // to bridge the tool history_id with its dom bubble (allocated earlier in
    // on_tool_call_pending).  Needed so tool-call pair-delete can DOM-remove
    // the tool rows alongside the parent assistant.
    int pending_tool_history_id_ = 0;
    // history_id -> dom_id, populated on on_history_message_added and consumed
    // on on_history_message_deleted so we can DOM-remove the right bubble.
    std::unordered_map<int, int> history_to_dom_;
    // Set by LocusFrame when ready to dispatch deletes through AgentCore.
    DeleteFn on_delete_message_;

    // WebView readiness: SetPage() is async in WebView2.
    bool                  page_ready_     = false;
    std::vector<wxString> pending_scripts_;
    bool                  in_run_script_  = false;
};

} // namespace locus
