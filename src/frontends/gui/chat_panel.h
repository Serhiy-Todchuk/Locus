#pragma once

#include "../../agent/agent_mode.h"
#include "../../core/frontend.h"
#include "slash_popup.h"

#include <wx/wx.h>
#include <wx/tglbtn.h>
#include <wx/webview.h>

#include <nlohmann/json.hpp>

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
//   [wxWebView   -- chat history, HTML/CSS rendered     ] (stretches)
//   [wxTextCtrl  -- multiline input, Enter=send         ] (fixed height)
//   [footer bar  -- context gauge + LOCUS.md chip       ] (fixed height)
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

    // Footer updates.
    void set_context_meter(int used, int limit,
                           int prompt_tokens = 0, int completion_tokens = 0,
                           int reserve_tokens = 0);
    // S5.D -- show/hide per-message token chips.
    void set_show_per_message_tokens(bool show);
    void set_generation_progress(int chars, int est_tokens);

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

private:
    void create_webview();
    void create_input();
    void create_footer();

    static std::string build_chat_html();

    void run_script(const wxString& js);

    static wxString js_escape(const wxString& s);
    static wxString user_text_to_html(const wxString& s);

    void on_flush_timer(wxTimerEvent& evt);
    void on_input_key(wxKeyEvent& evt);
    void on_input_text(wxCommandEvent& evt);
    void on_webview_navigating(wxWebViewEvent& evt);

    bool submit_current_input();
    void refresh_action_btn();

    // Collaborators (owned by unique_ptr so forward declarations above suffice).
    std::unique_ptr<ChatStreamRenderer> renderer_;
    std::unique_ptr<ChatFooterChips>    footer_chips_;
    std::unique_ptr<ChatPopups>         popups_;
    std::unique_ptr<ChatLinkHandler>    link_handler_;

    // Callbacks from LocusFrame.
    std::function<void(const std::string&)> on_send_;
    std::function<void()>                    on_compact_;
    std::function<void()>                    on_stop_;
    std::function<void()>                    on_undo_;
    std::function<void(AgentMode)>           on_mode_pick_;
    std::function<void(const std::string&)>  on_plan_decision_;
    std::function<bool(const std::string&, const std::string&)> on_slash_command_;
    std::function<void(const std::string&)>  on_mention_attach_;

    // Core widgets.
    wxWebView*    webview_       = nullptr;
    wxTextCtrl*   input_         = nullptr;
    wxButton*     compact_btn_   = nullptr;
    wxButton*     stop_btn_      = nullptr;
    wxButton*     undo_btn_      = nullptr;
    wxToggleButton* mode_chat_btn_    = nullptr;
    wxToggleButton* mode_plan_btn_    = nullptr;
    wxToggleButton* mode_execute_btn_ = nullptr;

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

    // Monotonic message div ID shared with ChatStreamRenderer (passed by ref).
    int message_id_ = 0;

    // S5.D -- per-message token chip state.
    bool show_per_message_tokens_   = true;
    int  last_completion_tokens_    = 0;

    // WebView readiness: SetPage() is async in WebView2.
    bool                  page_ready_     = false;
    std::vector<wxString> pending_scripts_;
    bool                  in_run_script_  = false;
};

} // namespace locus
