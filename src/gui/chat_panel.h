#pragma once

#include "../frontend.h"
#include "slash_popup.h"

#include <wx/wx.h>
#include <wx/webview.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace locus {

// Chat display + input panel. Center pane of the main frame.
//
// Layout (vertical):
//   [wxWebView   — chat history, HTML/CSS rendered     ] (stretches)
//   [wxTextCtrl  — multiline input, Enter=send         ] (fixed height)
//   [footer bar  — context gauge + LOCUS.md chip       ] (fixed height)
//
// Streaming: tokens are buffered and flushed to the WebView via a wxTimer
// every 33ms (~30fps). md4c converts accumulated markdown to HTML on each
// flush. Prism.js re-highlights code blocks on turn complete.

class ChatPanel : public wxPanel {
public:
    // on_send is called with the user's message text when Enter is pressed.
    // on_compact is called when the user clicks the manual compaction button.
    // on_stop is called when the user clicks Stop during generation.
    // on_undo is called when the user clicks the Undo button — should revert
    // the most recent checkpointed turn. Disabled while a turn is streaming.
    ChatPanel(wxWindow* parent,
              std::function<void(const std::string&)> on_send,
              std::function<void()> on_compact = nullptr,
              std::function<void()> on_stop = nullptr,
              std::function<void()> on_undo = nullptr);

    // -- Called by LocusFrame in response to agent events --

    void on_turn_start();
    void on_token(const wxString& token);
    void on_reasoning_token(const wxString& token);
    void on_turn_complete();
    void on_session_reset();
    void on_error(const wxString& message);

    // Tool call / result display (shown inline in chat).
    void on_tool_pending(const wxString& tool_name, const wxString& preview);
    void on_tool_result(const wxString& display);

    // Footer updates.
    void set_context_meter(int used, int limit);
    void set_locus_md_tokens(int tokens);

    // Attached-context chip (above input). Empty path hides it.
    // on_detach is invoked when the user clicks the chip's "✕" to detach.
    void set_attached_chip(const wxString& file_path);
    void set_on_detach(std::function<void()> cb);

    // Slash-command suggestions shown when the user types '/' in the input.
    // Items are typically CLI commands (reset, compact, ...) plus tool names.
    void set_slash_commands(std::vector<SlashItem> items);

    // Dispatch callback for GUI slash commands. Invoked when the user sends
    // a message that starts with '/' and the command name matches a known
    // GUI command. Returns true if handled (input is cleared, message is
    // NOT forwarded to the agent); false falls through to normal send.
    // Signature: (command_name_without_slash, rest_of_text_after_command)
    void set_on_slash_command(std::function<bool(const std::string&,
                                                 const std::string&)> cb);

    // Append a system-style note into the chat (used by /help).
    void append_system_note(const wxString& html);

private:
    void create_webview();
    void create_input();
    void create_footer();

    // Build the initial HTML page loaded into the WebView.
    static std::string build_chat_html();

    // Run JavaScript in the WebView. Wrapper for null-safety.
    void run_script(const wxString& js);

    // Escape a string for safe embedding in a JS string literal.
    static wxString js_escape(const wxString& s);

    // Timer callback: flush buffered tokens to the WebView.
    void on_flush_timer(wxTimerEvent& evt);

    // Input key handler: Enter=send, Shift+Enter=newline.
    void on_input_key(wxKeyEvent& evt);

    // Text change handler: show/hide/filter the slash-command popup.
    void on_input_text(wxCommandEvent& evt);

    // WebView navigation guard (block external URLs).
    void on_webview_navigating(wxWebViewEvent& evt);

    // Slash popup management.
    // The token in the input that triggers suggestions (e.g. "read" when the
    // user has typed "/read"). Empty if no such token at the cursor.
    wxString active_slash_token() const;
    void     update_slash_popup();
    void     hide_slash_popup();
    void     accept_slash_suggestion(const std::string& cmd_name);
    bool     slash_popup_visible() const;

    // Send the message to the agent (or dispatch as a GUI slash command).
    // Called from Enter-key handling. Returns true if handled.
    bool submit_current_input();

    std::function<void(const std::string&)> on_send_;
    std::function<void()> on_compact_;
    std::function<void()> on_stop_;
    std::function<void()> on_undo_;
    std::function<bool(const std::string&, const std::string&)> on_slash_command_;

    wxWebView*    webview_       = nullptr;
    wxTextCtrl*   input_         = nullptr;
    wxGauge*      ctx_gauge_     = nullptr;
    wxStaticText* ctx_label_     = nullptr;
    wxButton*     compact_btn_   = nullptr;
    wxButton*     stop_btn_      = nullptr;
    wxButton*     undo_btn_      = nullptr;
    wxStaticText* locus_chip_    = nullptr;

    // Attached-context chip row (sits between webview and input).
    wxPanel*      attach_panel_  = nullptr;  // the row container
    wxStaticText* attach_label_  = nullptr;  // "📎 path/to/file"
    wxButton*     attach_close_  = nullptr;  // small "✕" to detach
    std::function<void()> on_detach_;

    wxTimer       flush_timer_;

    // Token buffer (written from UI thread via on_token, read by timer).
    std::string   token_buffer_;
    std::string   current_response_;   // accumulated full response for md4c
    std::string   reasoning_buffer_;   // pending chain-of-thought tokens
    std::string   current_reasoning_;  // accumulated full reasoning text
    int           message_id_ = 0;     // monotonic ID for message divs
    int           assistant_id_ = 0;   // id of the assistant bubble for this turn
    int           reasoning_id_ = 0;   // id of the <details> thought block for this turn
    bool          streaming_  = false;  // true between turn_start and turn_complete

    // WebView readiness: SetPage() is async in WebView2.
    bool                         page_ready_ = false;
    std::vector<wxString>        pending_scripts_;

    // Slash-command suggestions.
    std::vector<SlashItem>       slash_commands_;
    std::unique_ptr<SlashPopup>  slash_popup_;
    bool                         slash_popup_shown_ = false;
};

} // namespace locus
