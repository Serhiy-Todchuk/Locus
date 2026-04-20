#pragma once

#include "../frontend.h"

#include <wx/wx.h>
#include <wx/webview.h>

#include <functional>
#include <mutex>
#include <string>

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
    ChatPanel(wxWindow* parent,
              std::function<void(const std::string&)> on_send,
              std::function<void()> on_compact = nullptr,
              std::function<void()> on_stop = nullptr);

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

    // WebView navigation guard (block external URLs).
    void on_webview_navigating(wxWebViewEvent& evt);

    std::function<void(const std::string&)> on_send_;
    std::function<void()> on_compact_;
    std::function<void()> on_stop_;

    wxWebView*    webview_       = nullptr;
    wxTextCtrl*   input_         = nullptr;
    wxGauge*      ctx_gauge_     = nullptr;
    wxStaticText* ctx_label_     = nullptr;
    wxButton*     compact_btn_   = nullptr;
    wxButton*     stop_btn_      = nullptr;
    wxStaticText* locus_chip_    = nullptr;

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
};

} // namespace locus
