#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <wx/string.h>

namespace locus {

// Owns all streaming-token display state for ChatPanel.
// Manages the assistant bubble, reasoning block, and "Thinking..." placeholder.
//
// run_js is a thin callback to ChatPanel::run_script (the only caller of
// WebView::RunScript). message_id is a reference to ChatPanel's monotonic
// counter so all message allocations stay globally ordered.
class ChatStreamRenderer {
public:
    using RunJsFn = std::function<void(const wxString&)>;

    ChatStreamRenderer(RunJsFn run_js, int& message_id);

    // Called when a new agent turn starts. Creates the "Thinking..." bubble.
    // ChatPanel is responsible for starting its flush timer after this call.
    void begin_turn();

    // Accumulate a streamed content token (called from on_token).
    void append_token(const wxString& token);

    // Accumulate a streamed reasoning token (called from on_reasoning_token).
    void append_reasoning_token(const wxString& token);

    // Called by ChatPanel's 33ms wxTimer. Returns true when there was work to
    // flush (reasoning buffer, token buffer, or placeholder tick). Returns false
    // on idle ticks so the caller can skip the RunScript overhead.
    bool flush();

    // Final flush and cleanup. Called when the agent signals turn complete.
    // ChatPanel is responsible for stopping the flush timer after this call.
    void end_turn();

    // Clear all state on session reset.
    void reset();

    // S5.Z #1: seal the currently open assistant/reasoning bubble before a tool
    // bubble is inserted. Post-tool tokens will open a fresh assistant bubble.
    // Called from ChatPanel::on_tool_pending.
    void seal_bubble();

    // State queries for ChatPanel.
    bool is_streaming() const { return streaming_; }
    bool is_waiting_for_first_token() const { return waiting_for_first_token_; }
    int  current_assistant_id() const { return assistant_id_; }

private:
    static wxString js_escape(const wxString& s);

    RunJsFn run_js_;
    int&    message_id_;

    std::string token_buffer_;
    std::string current_response_;
    std::string reasoning_buffer_;
    std::string current_reasoning_;
    int         assistant_id_            = 0;
    int         reasoning_id_            = 0;
    bool        streaming_               = false;
    bool        in_flush_                = false;
    bool        waiting_for_first_token_ = false;
    int         wait_ticks_              = 0;
    std::chrono::steady_clock::time_point turn_start_time_;
};

} // namespace locus
