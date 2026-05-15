#include "chat_stream_renderer.h"

#include "../markdown.h"

namespace locus {

ChatStreamRenderer::ChatStreamRenderer(RunJsFn run_js, int& message_id)
    : run_js_(std::move(run_js))
    , message_id_(message_id)
{}

void ChatStreamRenderer::begin_turn()
{
    streaming_ = true;
    current_response_.clear();
    token_buffer_.clear();
    current_reasoning_.clear();
    reasoning_buffer_.clear();
    reasoning_id_ = 0;

    waiting_for_first_token_ = true;
    wait_ticks_              = 0;
    turn_start_time_         = std::chrono::steady_clock::now();

    ++message_id_;
    assistant_id_ = message_id_;
    run_js_(wxString::Format(
        "addMsg(%d, 'msg-assistant streaming-cursor', "
        "'<em style=\"color:#888\">Thinking...</em>');",
        assistant_id_));
}

void ChatStreamRenderer::append_token(const wxString& token)
{
    // S5.Z #1 -- after seal_bubble zeroed assistant_id_, allocate a fresh bubble.
    if (assistant_id_ == 0) {
        ++message_id_;
        assistant_id_ = message_id_;
        run_js_(wxString::Format(
            "addMsg(%d, 'msg-assistant streaming-cursor', '');",
            assistant_id_));
    }
    if (waiting_for_first_token_) {
        waiting_for_first_token_ = false;
        run_js_(wxString::Format("setMsgHtml(%d, '');", assistant_id_));
    }
    token_buffer_.append(token.ToUTF8().data());
}

void ChatStreamRenderer::append_reasoning_token(const wxString& token)
{
    // S5.Z #1 -- same fresh-bubble logic as append_token.
    if (assistant_id_ == 0) {
        ++message_id_;
        assistant_id_ = message_id_;
        run_js_(wxString::Format(
            "addMsg(%d, 'msg-assistant streaming-cursor', '');",
            assistant_id_));
    }
    if (waiting_for_first_token_) {
        waiting_for_first_token_ = false;
        run_js_(wxString::Format("setMsgHtml(%d, '');", assistant_id_));
    }
    reasoning_buffer_.append(token.ToUTF8().data());
}

void ChatStreamRenderer::end_turn()
{
    waiting_for_first_token_ = false;

    // Final flush of any remaining reasoning tokens.
    if (!reasoning_buffer_.empty()) {
        if (reasoning_id_ == 0) {
            ++message_id_;
            reasoning_id_ = message_id_;
            run_js_(wxString::Format(
                "addReasoning(%d, %d);", reasoning_id_, assistant_id_));
        }
        current_reasoning_ += reasoning_buffer_;
        reasoning_buffer_.clear();
        run_js_(wxString::Format(
            "setReasoningBody(%d, %s);",
            reasoning_id_,
            "'" + js_escape(wxString::FromUTF8(current_reasoning_)) + "'"));
    }

    if (reasoning_id_ != 0) {
        run_js_(wxString::Format(
            "finalizeReasoning(%d, 'Thoughts');", reasoning_id_));
    }

    if (!token_buffer_.empty()) {
        current_response_ += token_buffer_;
        token_buffer_.clear();
    }

    // S5.Z #1 -- assistant_id_ may be 0 when the turn ends immediately after a
    // tool call and no further text token arrived. Also: a reasoning-only turn
    // leaves a whitespace-only bubble -- drop it rather than render it empty.
    auto visibly_empty = [](const std::string& s) {
        for (char c : s)
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
        return true;
    };

    if (assistant_id_ != 0) {
        if (visibly_empty(current_response_)) {
            run_js_(wxString::Format(
                "var d=document.getElementById('msg-%d');if(d)d.remove();",
                assistant_id_));
        } else {
            std::string html = markdown_to_html(current_response_);
            run_js_(wxString::Format(
                "setMsgHtml(%d, %s);",
                assistant_id_, "'" + js_escape(wxString::FromUTF8(html)) + "'"));
            run_js_(wxString::Format(
                "removeClassFromMsg(%d, 'streaming-cursor');", assistant_id_));
        }
    }

    run_js_("highlightAll();");
    streaming_ = false;
}

void ChatStreamRenderer::reset()
{
    current_response_.clear();
    token_buffer_.clear();
    current_reasoning_.clear();
    reasoning_buffer_.clear();
    streaming_               = false;
    waiting_for_first_token_ = false;
    assistant_id_            = 0;
    reasoning_id_            = 0;
}

void ChatStreamRenderer::seal_bubble()
{
    if (reasoning_id_ != 0) {
        if (!reasoning_buffer_.empty()) {
            current_reasoning_ += reasoning_buffer_;
            reasoning_buffer_.clear();
            run_js_(wxString::Format(
                "setReasoningBody(%d, %s);",
                reasoning_id_,
                "'" + js_escape(wxString::FromUTF8(current_reasoning_)) + "'"));
        }
        run_js_(wxString::Format(
            "finalizeReasoning(%d, 'Thoughts');", reasoning_id_));
        reasoning_id_ = 0;
        current_reasoning_.clear();
    }

    if (assistant_id_ != 0) {
        auto visibly_empty = [](const std::string& s) {
            for (char c : s)
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
            return true;
        };
        if (visibly_empty(current_response_) && visibly_empty(token_buffer_)) {
            run_js_(wxString::Format(
                "var d=document.getElementById('msg-%d');if(d)d.remove();",
                assistant_id_));
        } else {
            if (!token_buffer_.empty()) {
                current_response_ += token_buffer_;
                token_buffer_.clear();
            }
            std::string html = markdown_to_html(current_response_);
            run_js_(wxString::Format(
                "setMsgHtml(%d, %s);",
                assistant_id_,
                "'" + js_escape(wxString::FromUTF8(html)) + "'"));
            run_js_(wxString::Format(
                "removeClassFromMsg(%d, 'streaming-cursor');", assistant_id_));
        }
        assistant_id_ = 0;
        current_response_.clear();
        token_buffer_.clear();
    }
    waiting_for_first_token_ = false;
}

bool ChatStreamRenderer::flush()
{
    if (in_flush_) return false;
    struct Guard {
        bool& f;
        Guard(bool& b) : f(b) { f = true; }
        ~Guard() { f = false; }
    } g(in_flush_);

    if (waiting_for_first_token_) {
        ++wait_ticks_;
        if (wait_ticks_ % 30 == 1) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - turn_start_time_).count();
            run_js_(wxString::Format(
                "setMsgHtml(%d, '<em style=\"color:#888\">"
                "Thinking... (%llds)</em>');",
                assistant_id_, static_cast<long long>(elapsed)));
        }
        return true;
    }

    bool did_work = false;

    if (!reasoning_buffer_.empty()) {
        if (reasoning_id_ == 0) {
            ++message_id_;
            reasoning_id_ = message_id_;
            run_js_(wxString::Format(
                "addReasoning(%d, %d);", reasoning_id_, assistant_id_));
        }
        current_reasoning_ += reasoning_buffer_;
        reasoning_buffer_.clear();
        run_js_(wxString::Format(
            "setReasoningBody(%d, %s);",
            reasoning_id_,
            "'" + js_escape(wxString::FromUTF8(current_reasoning_)) + "'"));
        did_work = true;
    }

    if (!token_buffer_.empty()) {
        current_response_ += token_buffer_;
        token_buffer_.clear();
        std::string html = markdown_to_html(current_response_);
        run_js_(wxString::Format(
            "setMsgHtml(%d, %s);",
            assistant_id_,
            "'" + js_escape(wxString::FromUTF8(html)) + "'"));
        did_work = true;
    }

    return did_work;
}

wxString ChatStreamRenderer::js_escape(const wxString& s)
{
    wxString out;
    out.reserve(s.length() + 16);
    for (auto ch : s) {
        switch (ch.GetValue()) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '<':  out += "\\x3C"; break;
        default:   out += ch;      break;
        }
    }
    return out;
}

} // namespace locus
