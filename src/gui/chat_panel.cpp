#include "chat_panel.h"
#include "markdown.h"
#include "theme.h"

#include <spdlog/spdlog.h>

#include <wx/webview.h>

namespace locus {

// Timer interval for flushing token buffer to WebView (ms).
static constexpr int k_flush_interval_ms = 33;  // ~30fps

// ---------------------------------------------------------------------------
// Prism.js — minimal bundle for syntax highlighting.
// We embed the core + common languages inline to avoid external file deps.
// This is the CDN URL approach for now; a bundled copy can replace it later
// if offline-only operation matters for the GUI too.
// ---------------------------------------------------------------------------

static const char* k_prism_css_light_url =
    "https://cdnjs.cloudflare.com/ajax/libs/prism/1.29.0/themes/prism.min.css";
static const char* k_prism_css_dark_url =
    "https://cdnjs.cloudflare.com/ajax/libs/prism/1.29.0/themes/prism-tomorrow.min.css";
static const char* k_prism_js_url =
    "https://cdnjs.cloudflare.com/ajax/libs/prism/1.29.0/prism.min.js";
static const char* k_prism_autoloader_url =
    "https://cdnjs.cloudflare.com/ajax/libs/prism/1.29.0/plugins/autoloader/prism-autoloader.min.js";

// ---------------------------------------------------------------------------
// Chat HTML template
// ---------------------------------------------------------------------------

std::string ChatPanel::build_chat_html()
{
    std::string html = R"html(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<link rel="stylesheet" href=")html";
    html += k_prism_css_light_url;
    html += R"html(" media="(prefers-color-scheme: light)">
<link rel="stylesheet" href=")html";
    html += k_prism_css_dark_url;
    html += R"html(" media="(prefers-color-scheme: dark)">
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
    font-family: -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    font-size: 14px;
    line-height: 1.5;
    background: #fafafa;
    padding: 12px;
    overflow-y: auto;
}
#chat { display: flex; flex-direction: column; gap: 12px; }

.msg {
    max-width: 85%;
    padding: 10px 14px;
    border-radius: 12px;
    word-wrap: break-word;
    overflow-wrap: break-word;
}
.msg-user {
    align-self: flex-end;
    background: #0078d4;
    color: #fff;
    border-bottom-right-radius: 4px;
    white-space: pre-wrap;
}
.msg-assistant {
    align-self: flex-start;
    background: #fff;
    color: #1a1a1a;
    border: 1px solid #e0e0e0;
    border-bottom-left-radius: 4px;
}
.msg-tool {
    align-self: flex-start;
    background: #f0f4f8;
    color: #555;
    border: 1px solid #d0d8e0;
    border-radius: 8px;
    font-size: 12px;
    max-width: 90%;
}
.msg-tool .tool-name {
    font-weight: 600;
    color: #333;
}
.msg-tool .tool-preview {
    color: #666;
    font-style: italic;
}
.msg-tool .tool-result {
    margin-top: 6px;
    padding-top: 6px;
    border-top: 1px solid #d0d8e0;
    white-space: pre-wrap;
    font-family: "Cascadia Code", "Consolas", monospace;
    font-size: 12px;
    max-height: 200px;
    overflow-y: auto;
}
.msg-tool .tool-result-details {
    margin-top: 6px;
}
.msg-tool .tool-result-details summary {
    cursor: pointer;
    color: #555;
    font-size: 12px;
    user-select: none;
}
.msg-tool .tool-result-details pre {
    margin: 4px 0 0 0;
    padding: 6px;
    background: #f8f8f8;
    border: 1px solid #e0e0e0;
    border-radius: 4px;
    white-space: pre-wrap;
    font-family: "Cascadia Code", "Consolas", monospace;
    font-size: 12px;
    max-height: 200px;
    overflow-y: auto;
}
.msg-error {
    align-self: center;
    background: #fdecea;
    color: #b71c1c;
    border: 1px solid #f5c6cb;
    border-radius: 8px;
    font-size: 13px;
}

/* Markdown content styles */
.msg-assistant p { margin: 0.4em 0; }
.msg-assistant p:first-child { margin-top: 0; }
.msg-assistant p:last-child { margin-bottom: 0; }
.msg-assistant ul, .msg-assistant ol { margin: 0.4em 0; padding-left: 1.5em; }
.msg-assistant code {
    background: #f0f0f0;
    padding: 1px 4px;
    border-radius: 3px;
    font-family: "Cascadia Code", "Consolas", monospace;
    font-size: 13px;
}
.msg-assistant pre {
    margin: 0.5em 0;
    border-radius: 6px;
    overflow-x: auto;
}
.msg-assistant pre code {
    display: block;
    padding: 10px;
    background: #f5f5f5;
    border: 1px solid #e0e0e0;
    border-radius: 6px;
    font-size: 13px;
    line-height: 1.4;
}
.msg-assistant blockquote {
    border-left: 3px solid #0078d4;
    padding-left: 10px;
    margin: 0.4em 0;
    color: #555;
}
.msg-assistant table {
    border-collapse: collapse;
    margin: 0.5em 0;
}
.msg-assistant th, .msg-assistant td {
    border: 1px solid #ddd;
    padding: 4px 8px;
    text-align: left;
}
.msg-assistant th { background: #f5f5f5; font-weight: 600; }
.msg-assistant h1, .msg-assistant h2, .msg-assistant h3,
.msg-assistant h4, .msg-assistant h5, .msg-assistant h6 {
    margin: 0.6em 0 0.3em 0;
    line-height: 1.3;
}

.msg-reasoning {
    align-self: flex-start;
    background: transparent;
    color: #888;
    border: 1px dashed #d0d0d0;
    border-radius: 8px;
    font-size: 12px;
    max-width: 90%;
    padding: 6px 10px;
}
.msg-reasoning summary {
    cursor: pointer;
    color: #888;
    user-select: none;
    font-style: italic;
}
.msg-reasoning .reasoning-body {
    margin-top: 6px;
    padding-top: 6px;
    border-top: 1px dashed #d0d0d0;
    white-space: pre-wrap;
    font-family: "Cascadia Code", "Consolas", monospace;
    color: #777;
    max-height: 240px;
    overflow-y: auto;
}

.streaming-cursor::after {
    content: "\25CF";
    color: #0078d4;
    animation: blink 1s step-end infinite;
    margin-left: 2px;
}
@keyframes blink { 50% { opacity: 0; } }

/* -- Dark mode -- */
@media (prefers-color-scheme: dark) {
    body { background: #1e1e1e; color: #d4d4d4; }
    .msg-user { background: #264f78; color: #e0e0e0; }
    .msg-assistant {
        background: #2d2d2d; color: #d4d4d4;
        border-color: #444;
    }
    .msg-assistant code { background: #3a3a3a; color: #d4d4d4; }
    .msg-assistant pre code {
        background: #1e1e1e; border-color: #444; color: #d4d4d4;
    }
    .msg-assistant blockquote { border-left-color: #4a9eff; color: #aaa; }
    .msg-assistant th, .msg-assistant td { border-color: #555; }
    .msg-assistant th { background: #333; }
    .msg-tool {
        background: #252830; color: #aaa;
        border-color: #3a3f4b;
    }
    .msg-tool .tool-name { color: #ccc; }
    .msg-tool .tool-preview { color: #888; }
    .msg-tool .tool-result-details summary { color: #999; }
    .msg-tool .tool-result-details pre {
        background: #1e1e1e; border-color: #444; color: #ccc;
    }
    .msg-error {
        background: #3b1c1c; color: #f48771;
        border-color: #5a2d2d;
    }
    .msg-reasoning {
        border-color: #444; color: #888;
    }
    .msg-reasoning summary { color: #888; }
    .msg-reasoning .reasoning-body {
        border-top-color: #444; color: #999;
    }
    .streaming-cursor::after { color: #4a9eff; }
}
</style>
</head><body>
<div id="chat"></div>
<script>
function addMsg(id, cls, html) {
    var d = document.createElement('div');
    d.id = 'msg-' + id;
    d.className = 'msg ' + cls;
    d.innerHTML = html;
    document.getElementById('chat').appendChild(d);
    window.scrollTo(0, document.body.scrollHeight);
}
function setMsgHtml(id, html) {
    var d = document.getElementById('msg-' + id);
    if (d) { d.innerHTML = html; window.scrollTo(0, document.body.scrollHeight); }
}
function addClassToMsg(id, cls) {
    var d = document.getElementById('msg-' + id);
    if (d) d.classList.add(cls);
}
function removeClassFromMsg(id, cls) {
    var d = document.getElementById('msg-' + id);
    if (d) d.classList.remove(cls);
}
function highlightAll() {
    if (typeof Prism !== 'undefined') Prism.highlightAll();
}
function clearChat() {
    document.getElementById('chat').innerHTML = '';
}
function addReasoning(id, beforeId) {
    var d = document.createElement('details');
    d.id = 'msg-' + id;
    d.className = 'msg msg-reasoning';
    d.open = true;
    d.innerHTML = '<summary>Thinking\u2026</summary><div class="reasoning-body"></div>';
    var chat = document.getElementById('chat');
    var before = document.getElementById('msg-' + beforeId);
    if (before) chat.insertBefore(d, before);
    else chat.appendChild(d);
    window.scrollTo(0, document.body.scrollHeight);
}
function setReasoningBody(id, text) {
    var d = document.getElementById('msg-' + id);
    if (d) {
        var body = d.querySelector('.reasoning-body');
        if (body) { body.textContent = text; }
        if (d.open) window.scrollTo(0, document.body.scrollHeight);
    }
}
function finalizeReasoning(id, label) {
    var d = document.getElementById('msg-' + id);
    if (d) {
        d.open = false;
        var s = d.querySelector('summary');
        if (s && label) s.textContent = label;
    }
}
</script>
<script src=")html";
    html += k_prism_js_url;
    html += R"html(" async></script>
<script src=")html";
    html += k_prism_autoloader_url;
    html += R"html(" async></script>
</body></html>)html";

    return html;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ChatPanel::ChatPanel(wxWindow* parent,
                     std::function<void(const std::string&)> on_send,
                     std::function<void()> on_compact,
                     std::function<void()> on_stop)
    : wxPanel(parent, wxID_ANY)
    , on_send_(std::move(on_send))
    , on_compact_(std::move(on_compact))
    , on_stop_(std::move(on_stop))
    , flush_timer_(this)
{
    create_webview();
    create_input();
    create_footer();

    // Main vertical layout.
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(webview_, 1, wxEXPAND);
    sizer->Add(input_,   0, wxEXPAND | wxTOP, 2);

    // Footer bar.
    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    footer->Add(ctx_gauge_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(ctx_label_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(compact_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(stop_btn_, 0, wxALIGN_CENTER_VERTICAL);
    footer->AddStretchSpacer();
    footer->Add(locus_chip_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(footer, 0, wxEXPAND | wxALL, 4);

    SetSizer(sizer);

    // Bind timer.
    Bind(wxEVT_TIMER, &ChatPanel::on_flush_timer, this);
}

void ChatPanel::create_webview()
{
    webview_ = wxWebView::New(this, wxID_ANY);
    webview_->SetPage(wxString::FromUTF8(build_chat_html()), "about:blank");

    // Block navigation to external URLs.
    webview_->Bind(wxEVT_WEBVIEW_NAVIGATING, &ChatPanel::on_webview_navigating, this);

    // SetPage() is async in WebView2 — wait for loaded event before running JS.
    webview_->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent&) {
        if (page_ready_) return;  // only handle the first load
        page_ready_ = true;
        size_t n = pending_scripts_.size();
        for (auto& js : pending_scripts_)
            webview_->RunScript(js);
        pending_scripts_.clear();
        spdlog::info("WebView page loaded, flushed {} queued scripts", n);
    });
}

void ChatPanel::create_input()
{
    input_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                            wxDefaultPosition, wxSize(-1, 60),
                            wxTE_MULTILINE | wxTE_PROCESS_ENTER | wxTE_RICH2);
    input_->SetHint("Type a message... (Enter to send, Shift+Enter for newline)");
    input_->SetBackgroundColour(theme::text_bg());
    input_->SetForegroundColour(theme::text_fg());
    input_->Bind(wxEVT_KEY_DOWN, &ChatPanel::on_input_key, this);
}

void ChatPanel::create_footer()
{
    ctx_gauge_ = new wxGauge(this, wxID_ANY, 100,
                             wxDefaultPosition, wxSize(120, 16));
    ctx_label_ = new wxStaticText(this, wxID_ANY, "ctx: 0/0",
                                     wxDefaultPosition, wxSize(150, -1));
    compact_btn_ = new wxButton(this, wxID_ANY, "Compact",
                                wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    compact_btn_->SetToolTip("Open context compaction dialog");
    compact_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_compact_) on_compact_();
    });
    stop_btn_ = new wxButton(this, wxID_ANY, "Stop",
                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    stop_btn_->SetToolTip("Stop the current generation");
    stop_btn_->Disable();
    stop_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_stop_) on_stop_();
    });
    locus_chip_ = new wxStaticText(this, wxID_ANY, "");
}

// ---------------------------------------------------------------------------
// Public API — called from LocusFrame event handlers
// ---------------------------------------------------------------------------

void ChatPanel::on_turn_start()
{
    streaming_ = true;
    current_response_.clear();
    token_buffer_.clear();
    current_reasoning_.clear();
    reasoning_buffer_.clear();
    reasoning_id_ = 0;

    stop_btn_->Enable();

    // Create an empty assistant message div.
    ++message_id_;
    assistant_id_ = message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-assistant streaming-cursor', '');", assistant_id_));

    flush_timer_.Start(k_flush_interval_ms);
}

void ChatPanel::on_token(const wxString& token)
{
    // Accumulate into buffer. on_flush_timer will render it.
    token_buffer_.append(token.ToUTF8().data());
}

void ChatPanel::on_reasoning_token(const wxString& token)
{
    reasoning_buffer_.append(token.ToUTF8().data());
}

void ChatPanel::on_turn_complete()
{
    flush_timer_.Stop();

    // Final flush of any remaining reasoning tokens.
    if (!reasoning_buffer_.empty()) {
        if (reasoning_id_ == 0) {
            ++message_id_;
            reasoning_id_ = message_id_;
            run_script(wxString::Format(
                "addReasoning(%d, %d);", reasoning_id_, assistant_id_));
        }
        current_reasoning_ += reasoning_buffer_;
        reasoning_buffer_.clear();
        run_script(wxString::Format(
            "setReasoningBody(%d, %s);",
            reasoning_id_,
            "'" + js_escape(wxString::FromUTF8(current_reasoning_)) + "'"));
    }

    // Collapse reasoning block: "Thinking…" → "Thoughts".
    if (reasoning_id_ != 0) {
        run_script(wxString::Format(
            "finalizeReasoning(%d, 'Thoughts');", reasoning_id_));
    }

    // Final flush of any remaining tokens.
    if (!token_buffer_.empty()) {
        current_response_ += token_buffer_;
        token_buffer_.clear();
    }

    // Final render with md4c.
    std::string html = markdown_to_html(current_response_);
    run_script(wxString::Format(
        "setMsgHtml(%d, %s);",
        assistant_id_, "'" + js_escape(wxString::FromUTF8(html)) + "'"));

    // Remove streaming cursor.
    run_script(wxString::Format(
        "removeClassFromMsg(%d, 'streaming-cursor');", assistant_id_));

    // Highlight code blocks.
    run_script("highlightAll();");

    streaming_ = false;
    stop_btn_->Disable();
    // Use SetEditable (not Enable) to preserve the custom dark background —
    // Disable() forces Windows' light "disabled control" colour.
    input_->SetEditable(true);
    input_->SetFocus();
}

void ChatPanel::on_session_reset()
{
    run_script("clearChat();");
    current_response_.clear();
    token_buffer_.clear();
    current_reasoning_.clear();
    reasoning_buffer_.clear();
    streaming_ = false;
    stop_btn_->Disable();
    message_id_ = 0;
    assistant_id_ = 0;
    reasoning_id_ = 0;
}

void ChatPanel::on_error(const wxString& message)
{
    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-error', %s);",
        message_id_, "'" + js_escape(message) + "'"));
}

void ChatPanel::on_tool_pending(const wxString& tool_name, const wxString& preview)
{
    ++message_id_;
    wxString content = "<span class=\"tool-name\">" + js_escape(tool_name) + "</span>";
    if (!preview.empty())
        content += "<br><span class=\"tool-preview\">" + js_escape(preview) + "</span>";

    run_script(wxString::Format(
        "addMsg(%d, 'msg-tool', %s);",
        message_id_, "'" + js_escape(content) + "'"));
}

void ChatPanel::on_tool_result(const wxString& display)
{
    if (display.empty()) return;

    // Truncate long results for display.
    wxString truncated = display;
    if (truncated.length() > 500)
        truncated = truncated.Left(500) + "... (" +
                    wxString::Format("%zu", display.length() - 500) + " chars truncated)";

    // Append result to the last tool message as a collapsible <details> block.
    run_script(wxString::Format(
        "var d=document.getElementById('msg-%d');"
        "if(d){var det=document.createElement('details');"
        "det.className='tool-result-details';"
        "var sum=document.createElement('summary');"
        "sum.textContent='Result';"
        "det.appendChild(sum);"
        "var pre=document.createElement('pre');"
        "pre.className='tool-result';pre.textContent=%s;"
        "det.appendChild(pre);"
        "d.appendChild(det);"
        "window.scrollTo(0,document.body.scrollHeight);}",
        message_id_, "'" + js_escape(truncated) + "'"));
}

void ChatPanel::set_context_meter(int used, int limit)
{
    int pct = (limit > 0) ? (used * 100 / limit) : 0;
    ctx_gauge_->SetValue(std::min(pct, 100));
    ctx_label_->SetLabel(wxString::Format("ctx: %d/%d (%d%%)", used, limit, pct));

    // Color: green < 60%, yellow 60-80%, red > 80%.
    if (pct < 60)
        ctx_gauge_->SetForegroundColour(wxColour(76, 175, 80));
    else if (pct < 80)
        ctx_gauge_->SetForegroundColour(wxColour(255, 193, 7));
    else
        ctx_gauge_->SetForegroundColour(wxColour(244, 67, 54));
}

void ChatPanel::set_locus_md_tokens(int tokens)
{
    if (tokens > 0)
        locus_chip_->SetLabel(wxString::Format("[LOCUS.md: %d tk]", tokens));
    else
        locus_chip_->SetLabel("");
}

// ---------------------------------------------------------------------------
// Timer: flush tokens to WebView
// ---------------------------------------------------------------------------

void ChatPanel::on_flush_timer(wxTimerEvent& /*evt*/)
{
    // Flush reasoning first (it arrives before content for Gemma etc.).
    if (!reasoning_buffer_.empty()) {
        if (reasoning_id_ == 0) {
            ++message_id_;
            reasoning_id_ = message_id_;
            // Insert reasoning block BEFORE the assistant bubble.
            run_script(wxString::Format(
                "addReasoning(%d, %d);", reasoning_id_, assistant_id_));
        }
        current_reasoning_ += reasoning_buffer_;
        reasoning_buffer_.clear();
        run_script(wxString::Format(
            "setReasoningBody(%d, %s);",
            reasoning_id_,
            "'" + js_escape(wxString::FromUTF8(current_reasoning_)) + "'"));
    }

    if (token_buffer_.empty()) return;

    current_response_ += token_buffer_;
    token_buffer_.clear();

    // Re-render full accumulated response through md4c.
    std::string html = markdown_to_html(current_response_);
    run_script(wxString::Format(
        "setMsgHtml(%d, %s);",
        assistant_id_,
        "'" + js_escape(wxString::FromUTF8(html)) + "'"));
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

void ChatPanel::on_input_key(wxKeyEvent& evt)
{
    if (evt.GetKeyCode() == WXK_RETURN && !evt.ShiftDown()) {
        wxString text = input_->GetValue().Trim().Trim(false);
        if (text.empty()) return;

        input_->Clear();

        // Add user message bubble.
        ++message_id_;
        run_script(wxString::Format(
            "addMsg(%d, 'msg-user', %s);",
            message_id_, "'" + js_escape(text) + "'"));

        // Block typing while agent is working, but keep the dark styling.
        // (Enable/Disable on Windows resets colours to the light system defaults.)
        input_->SetEditable(false);

        // Fire callback.
        if (on_send_)
            on_send_(text.ToStdString(wxConvUTF8));
    } else {
        evt.Skip();
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void ChatPanel::run_script(const wxString& js)
{
    if (!webview_) return;

    if (page_ready_) {
        webview_->RunScript(js);
    } else {
        pending_scripts_.push_back(js);
    }
}

wxString ChatPanel::js_escape(const wxString& s)
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
        case '<':  out += "\\x3C"; break;  // prevent </script> injection
        default:   out += ch;     break;
        }
    }
    return out;
}

void ChatPanel::on_webview_navigating(wxWebViewEvent& evt)
{
    wxString url = evt.GetURL();

    // Allow the initial SetPage() load — before page_ready_, let everything through.
    if (!page_ready_)
        return;

    // After page is loaded, allow javascript: scheme only.
    if (url.StartsWith("javascript:"))
        return;

    // Block all external navigation (user clicking links in chat).
    spdlog::trace("WebView navigation blocked: {}", url.ToStdString());
    evt.Veto();
}

} // namespace locus
