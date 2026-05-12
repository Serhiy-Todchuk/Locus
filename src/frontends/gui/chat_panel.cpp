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

/* S4.D plan bubble */
.msg-plan {
    align-self: stretch;
    background: #fff8e6;
    color: #3a2f00;
    border: 1px solid #ffd66b;
    border-left: 4px solid #ffb300;
    border-radius: 8px;
    font-size: 13px;
    padding: 10px 12px;
    max-width: 100%;
}
.msg-plan .plan-title {
    font-weight: 600;
    font-size: 14px;
    margin-bottom: 2px;
}
.msg-plan .plan-summary {
    color: #6b5800;
    font-size: 12px;
    margin-bottom: 8px;
}
.msg-plan ol.plan-steps {
    list-style: none;
    padding-left: 0;
    margin: 0 0 8px 0;
}
.msg-plan li.plan-step {
    padding: 4px 0;
    line-height: 1.45;
    display: flex;
    align-items: flex-start;
    gap: 6px;
}
.msg-plan li.plan-step .step-glyph {
    flex: 0 0 auto;
    width: 18px;
    text-align: center;
    color: #b07800;
}
.msg-plan li.plan-step.done    .step-glyph { color: #2e7d32; }
.msg-plan li.plan-step.failed  .step-glyph { color: #c62828; }
.msg-plan li.plan-step.in_progress .step-glyph {
    color: #1976d2;
    animation: spin 1s linear infinite;
}
@keyframes spin { 100% { transform: rotate(360deg); } }
.msg-plan li.plan-step .step-tools {
    color: #888;
    font-size: 11px;
    margin-left: 4px;
}
.msg-plan li.plan-step .step-notes {
    color: #555;
    font-size: 11px;
    margin-top: 2px;
    font-style: italic;
}
.msg-plan .plan-actions {
    margin-top: 6px;
    display: flex;
    gap: 8px;
}
.msg-plan .plan-actions a {
    text-decoration: none;
    padding: 4px 10px;
    border-radius: 4px;
    font-size: 12px;
    font-weight: 600;
}
.msg-plan .plan-actions a.approve {
    background: #2e7d32;
    color: #fff;
}
.msg-plan .plan-actions a.reject {
    background: #fff;
    color: #c62828;
    border: 1px solid #c62828;
}
.msg-plan .plan-decided {
    margin-top: 6px;
    color: #6b5800;
    font-size: 12px;
    font-style: italic;
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

// ---- S4.D plan bubble helpers ---------------------------------------------

function planGlyph(status) {
    if (status === 'done')        return '✓';   // check
    if (status === 'failed')      return '✗';   // x
    if (status === 'in_progress') return '⧗';   // hourglass-ish
    return '○';                                  // pending: hollow circle
}

function escapeHtml(s) {
    return (s || '').replace(/&/g, '&amp;').replace(/</g, '&lt;')
                    .replace(/>/g, '&gt;').replace(/"/g, '&quot;')
                    .replace(/'/g, '&#39;');
}

// Add a plan bubble to the chat. plan_json is a JSON string with the shape
// produced by plan_to_json (Plan struct in agent_mode.h):
//   { id, title, summary, steps: [{description, tools_needed[], status, notes?}] }
function addPlanMsg(id, plan_json) {
    var plan;
    try { plan = JSON.parse(plan_json); }
    catch (e) { return; }

    var d = document.createElement('div');
    d.id = 'msg-' + id;
    d.className = 'msg msg-plan';
    d.setAttribute('data-plan-id', plan.id || '');

    var html = '';
    if (plan.title)
        html += '<div class="plan-title">Plan: ' + escapeHtml(plan.title) + '</div>';
    if (plan.summary)
        html += '<div class="plan-summary">' + escapeHtml(plan.summary) + '</div>';
    html += '<ol class="plan-steps">';
    var steps = plan.steps || [];
    for (var i = 0; i < steps.length; ++i) {
        var s = steps[i];
        var status = s.status || 'pending';
        html += '<li class="plan-step ' + status +
                '" data-step-idx="' + i + '">';
        html += '<span class="step-glyph">' + planGlyph(status) + '</span>';
        html += '<span class="step-body">' +
                (i + 1) + '. ' + escapeHtml(s.description || '');
        if (s.tools_needed && s.tools_needed.length) {
            html += '<span class="step-tools"> &mdash; uses ' +
                    escapeHtml(s.tools_needed.join(', ')) + '</span>';
        }
        if (s.notes) {
            html += '<div class="step-notes">' + escapeHtml(s.notes) + '</div>';
        }
        html += '</span></li>';
    }
    html += '</ol>';
    // Approve / Reject buttons. Routed through wxEVT_WEBVIEW_NAVIGATING via
    // the locus:// scheme; the chat panel intercepts and dispatches to
    // AgentCore::approve_plan / reject_plan.
    html += '<div class="plan-actions" data-actions-for="' +
            (plan.id || '') + '">';
    html += '<a class="approve" href="locus://plan-approve/' +
            (plan.id || '') + '">Approve &amp; execute</a>';
    html += '<a class="reject" href="locus://plan-reject/' +
            (plan.id || '') + '">Reject</a>';
    html += '</div>';

    d.innerHTML = html;
    document.getElementById('chat').appendChild(d);
    window.scrollTo(0, document.body.scrollHeight);
}

// Flip a step's status class + glyph in an existing plan bubble.
function updatePlanStep(msgId, stepIdx, status, notes) {
    var d = document.getElementById('msg-' + msgId);
    if (!d) return;
    var step = d.querySelector('li.plan-step[data-step-idx="' + stepIdx + '"]');
    if (!step) return;
    step.className = 'plan-step ' + (status || 'pending');
    var glyph = step.querySelector('.step-glyph');
    if (glyph) glyph.textContent = planGlyph(status);
    if (notes) {
        var existing = step.querySelector('.step-notes');
        if (existing) existing.textContent = notes;
        else {
            var body = step.querySelector('.step-body');
            if (body) {
                var n = document.createElement('div');
                n.className = 'step-notes';
                n.textContent = notes;
                body.appendChild(n);
            }
        }
    }
    window.scrollTo(0, document.body.scrollHeight);
}

// Mark the plan bubble as decided (approved / rejected / completed). Hides
// the action buttons and replaces them with a status line.
function setPlanDecided(msgId, label) {
    var d = document.getElementById('msg-' + msgId);
    if (!d) return;
    var actions = d.querySelector('.plan-actions');
    if (actions) actions.remove();
    var existing = d.querySelector('.plan-decided');
    if (existing) existing.textContent = label;
    else {
        var n = document.createElement('div');
        n.className = 'plan-decided';
        n.textContent = label;
        d.appendChild(n);
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
                     std::function<void()> on_stop,
                     std::function<void()> on_undo,
                     std::function<void(AgentMode)> on_mode_pick,
                     std::function<void(const std::string&)> on_plan_decision)
    : wxPanel(parent, wxID_ANY)
    , on_send_(std::move(on_send))
    , on_compact_(std::move(on_compact))
    , on_stop_(std::move(on_stop))
    , on_undo_(std::move(on_undo))
    , on_mode_pick_(std::move(on_mode_pick))
    , on_plan_decision_(std::move(on_plan_decision))
    , flush_timer_(this)
{
    create_webview();
    create_input();
    create_footer();

    // S4.D mode switcher (Chat / Plan / Execute) -- a row of mutually
    // exclusive toggles above the input. Disabled while a turn is streaming
    // (mode change requests are queued anyway, but the visual feedback
    // matches the "input disabled" state).
    auto mk_toggle = [this](const wxString& label, AgentMode m,
                            const wxString& tip) {
        auto* btn = new wxToggleButton(this, wxID_ANY, label,
                                       wxDefaultPosition, wxDefaultSize,
                                       wxBU_EXACTFIT);
        btn->SetToolTip(tip);
        btn->Bind(wxEVT_TOGGLEBUTTON, [this, m, btn](wxCommandEvent&) {
            // Re-enforce mutual exclusivity: clicking an already-active
            // toggle is a no-op (keep it down), clicking a different one
            // releases the others.
            if (!btn->GetValue()) {
                btn->SetValue(true);  // can't untoggle by clicking the same
                return;
            }
            if (mode_chat_btn_    && mode_chat_btn_    != btn) mode_chat_btn_->SetValue(false);
            if (mode_plan_btn_    && mode_plan_btn_    != btn) mode_plan_btn_->SetValue(false);
            if (mode_execute_btn_ && mode_execute_btn_ != btn) mode_execute_btn_->SetValue(false);
            if (on_mode_pick_) on_mode_pick_(m);
        });
        return btn;
    };

    mode_chat_btn_ = mk_toggle("Chat", AgentMode::chat,
        "Default: full tool catalog, no plan workflow.");
    mode_plan_btn_ = mk_toggle("Plan", AgentMode::plan,
        "Plan mode: model proposes a structured plan; you Approve to execute.");
    mode_execute_btn_ = mk_toggle("Execute", AgentMode::execute,
        "Execute mode: full tool catalog plus mark_step_done. "
        "Usually entered automatically after Approve.");
    mode_chat_btn_->SetValue(true);  // default selection

    // Attached-context chip row (between chat history and input).
    attach_panel_ = new wxPanel(this, wxID_ANY);
    attach_label_ = new wxStaticText(attach_panel_, wxID_ANY, "");
    // U+1F4CE PAPERCLIP. Use a font with emoji coverage on Windows.
    attach_close_ = new wxButton(attach_panel_, wxID_ANY, "x",
                                 wxDefaultPosition, wxSize(22, 22),
                                 wxBU_EXACTFIT);
    attach_close_->SetToolTip("Detach file from conversation context");
    attach_close_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_detach_) on_detach_();
    });
    auto* attach_sizer = new wxBoxSizer(wxHORIZONTAL);
    attach_sizer->Add(attach_label_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    attach_sizer->Add(attach_close_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
    attach_panel_->SetSizer(attach_sizer);
    attach_panel_->Hide();  // shown when set_attached_chip() is called

    // Mode switcher row (S4.D) sits between attach chip and input.
    auto* mode_row = new wxBoxSizer(wxHORIZONTAL);
    mode_row->Add(new wxStaticText(this, wxID_ANY, "Mode:"),
                  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    mode_row->Add(mode_chat_btn_,    0, wxRIGHT, 2);
    mode_row->Add(mode_plan_btn_,    0, wxRIGHT, 2);
    mode_row->Add(mode_execute_btn_, 0);

    // Main vertical layout.
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(webview_, 1, wxEXPAND);
    sizer->Add(attach_panel_, 0, wxEXPAND | wxTOP | wxBOTTOM, 2);
    sizer->Add(mode_row, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 2);
    sizer->Add(input_,   0, wxEXPAND | wxTOP, 2);

    // Footer bar.
    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    footer->Add(ctx_gauge_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(ctx_label_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(compact_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(undo_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(stop_btn_, 0, wxALIGN_CENTER_VERTICAL);
    footer->AddStretchSpacer();
    footer->Add(plan_chip_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    footer->Add(commit_chip_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
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
    // No SetHint() — wx's multiline+RICH2 hint seeds real text content on
    // Windows rather than painting an overlay, so the "placeholder" becomes
    // editable and has to be manually deleted. Use a tooltip instead for
    // discoverability; the keystroke help lives in the status bar / footer.
    input_->SetToolTip(
        "Type a message and press Enter to send.\n"
        "Shift+Enter inserts a newline. Type '/' for commands.");
    input_->SetBackgroundColour(theme::text_bg());
    input_->SetForegroundColour(theme::text_fg());
    input_->Bind(wxEVT_KEY_DOWN, &ChatPanel::on_input_key, this);
    input_->Bind(wxEVT_TEXT, &ChatPanel::on_input_text, this);
    // Hide the popup when the text control loses focus (e.g. user tabs away
    // or clicks elsewhere in the window). Clicking the popup's listbox also
    // triggers kill_focus, but by then on_accept has already fired on
    // left-up, so dismissing here is safe.
    input_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& evt) {
        hide_slash_popup();
        evt.Skip();
    });
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
    undo_btn_ = new wxButton(this, wxID_ANY, "Undo",
                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    undo_btn_->SetToolTip("Revert files mutated by the most recent turn");
    undo_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_undo_) on_undo_();
    });
    locus_chip_ = new wxStaticText(this, wxID_ANY, "");
    plan_chip_  = new wxStaticText(this, wxID_ANY, "");
    plan_chip_->Hide();  // shown only while a plan is active
    commit_chip_ = new wxStaticText(this, wxID_ANY, "");
    commit_chip_->Hide(); // shown only when git auto-commit is on AND fired
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

    waiting_for_first_token_ = true;
    wait_ticks_              = 0;
    turn_start_time_         = std::chrono::steady_clock::now();

    stop_btn_->Enable();
    if (undo_btn_) undo_btn_->Disable();

    // Create the assistant bubble pre-populated with a "Thinking..."
    // placeholder. The flush timer ticks the elapsed-seconds counter once
    // per second until the first text or reasoning token arrives.
    ++message_id_;
    assistant_id_ = message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-assistant streaming-cursor', "
        "'<em style=\"color:#888\">Thinking...</em>');",
        assistant_id_));

    flush_timer_.Start(k_flush_interval_ms);
}

void ChatPanel::on_token(const wxString& token)
{
    if (waiting_for_first_token_) {
        waiting_for_first_token_ = false;
        // Wipe the "Thinking..." placeholder so the streaming text starts
        // from a clean bubble. on_flush_timer will rebuild the body from
        // current_response_ via md4c on its next tick.
        run_script(wxString::Format("setMsgHtml(%d, '');", assistant_id_));
    }
    // Accumulate into buffer. on_flush_timer will render it.
    token_buffer_.append(token.ToUTF8().data());
}

void ChatPanel::on_reasoning_token(const wxString& token)
{
    if (waiting_for_first_token_) {
        waiting_for_first_token_ = false;
        // Reasoning lands in the separate <details> block above the assistant
        // bubble, so the bubble itself goes back to empty until a content
        // token arrives.
        run_script(wxString::Format("setMsgHtml(%d, '');", assistant_id_));
    }
    reasoning_buffer_.append(token.ToUTF8().data());
}

void ChatPanel::on_turn_complete()
{
    flush_timer_.Stop();
    waiting_for_first_token_ = false;

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
    if (undo_btn_) undo_btn_->Enable();
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
    waiting_for_first_token_ = false;
    stop_btn_->Disable();
    message_id_ = 0;
    assistant_id_ = 0;
    reasoning_id_ = 0;
    tool_call_msg_ids_.clear();
    // S4.D: drop plan state + flip the mode switcher back to Chat.
    plan_msg_ids_.clear();
    current_plan_id_.clear();
    current_plan_total_steps_ = 0;
    current_plan_done_steps_  = 0;
    current_plan_step_label_.clear();
    if (plan_chip_) {
        plan_chip_->SetLabel("");
        plan_chip_->Hide();
        Layout();
    }
    on_mode_changed(AgentMode::chat);
}

// ---------------------------------------------------------------------------
// S4.D plan-mode display
// ---------------------------------------------------------------------------

void ChatPanel::on_mode_changed(AgentMode mode)
{
    // Flip toggles WITHOUT firing the EVT_TOGGLEBUTTON handler (which would
    // re-call on_mode_pick_ and bounce back to AgentCore). SetValue is the
    // wx-idiomatic non-emitting setter.
    if (mode_chat_btn_)    mode_chat_btn_->SetValue(mode == AgentMode::chat);
    if (mode_plan_btn_)    mode_plan_btn_->SetValue(mode == AgentMode::plan);
    if (mode_execute_btn_) mode_execute_btn_->SetValue(mode == AgentMode::execute);

    // Hide the plan chip when we drop back to chat with no plan in flight.
    if (mode == AgentMode::chat && plan_chip_) {
        plan_chip_->SetLabel("");
        plan_chip_->Hide();
        Layout();
    }
}

namespace {

// Format a "Plan: X/Y -- next-step-label" footer chip text. Truncates long
// step descriptions to keep the footer visually balanced.
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

void ChatPanel::on_plan_proposed(const wxString& plan_json)
{
    // Parse just enough to hydrate the chip + remember the message id.
    // Full rendering is done JS-side via addPlanMsg.
    int total = 0;
    std::string id;
    std::string first_step;
    try {
        auto j = nlohmann::json::parse(plan_json.utf8_string());
        id = j.value("id", "");
        if (j.contains("steps") && j["steps"].is_array()) {
            total = static_cast<int>(j["steps"].size());
            if (total > 0)
                first_step = j["steps"][0].value("description", "");
        }
    } catch (const nlohmann::json::exception&) {
        spdlog::warn("ChatPanel: malformed plan JSON; rendering raw");
    }

    ++message_id_;
    if (!id.empty()) plan_msg_ids_[id] = message_id_;
    current_plan_id_           = id;
    current_plan_total_steps_  = total;
    current_plan_done_steps_   = 0;
    current_plan_step_label_   = first_step;

    if (plan_chip_) {
        plan_chip_->SetLabel(format_plan_chip(0, total, first_step));
        plan_chip_->Show();
        Layout();
    }

    // JS-side rendering. addPlanMsg handles the heavy lifting (status
    // glyphs, Approve/Reject buttons, escaping). The JSON is js-escaped
    // so it round-trips cleanly through the JS string literal.
    run_script(wxString::Format("addPlanMsg(%d, '%s');",
                                message_id_, js_escape(plan_json)));
}

void ChatPanel::on_plan_step_advanced(const wxString& plan_id, int step_idx,
                                       const wxString& status,
                                       const wxString& notes)
{
    auto it = plan_msg_ids_.find(std::string(plan_id.utf8_string()));
    if (it == plan_msg_ids_.end()) {
        spdlog::warn("ChatPanel: step advance for unknown plan id '{}'",
                     plan_id.ToStdString());
        return;
    }
    int msg_id = it->second;

    run_script(wxString::Format(
        "updatePlanStep(%d, %d, '%s', '%s');",
        msg_id, step_idx, js_escape(status), js_escape(notes)));

    // Bump the footer chip on done/failed.
    if (plan_id == wxString::FromUTF8(current_plan_id_)
        && (status == "done" || status == "failed")) {
        ++current_plan_done_steps_;
        // Try to surface the NEXT pending step as the chip label so the user
        // sees "what's coming up" rather than "what just finished".
        // Without re-parsing the plan we don't know the next description, so
        // for now reuse the previously-shown label until the next on_plan_
        // proposed clears it. (Phase 2-of-2 polish if the shipped UI feels
        // stale.)
        if (plan_chip_) {
            plan_chip_->SetLabel(
                format_plan_chip(current_plan_done_steps_,
                                  current_plan_total_steps_,
                                  current_plan_step_label_));
            Layout();
        }
    }
}

void ChatPanel::on_plan_completed(const wxString& plan_id, bool success)
{
    auto it = plan_msg_ids_.find(std::string(plan_id.utf8_string()));
    if (it != plan_msg_ids_.end()) {
        wxString label = success ? "Plan completed."
                                  : "Plan completed with failures.";
        run_script(wxString::Format(
            "setPlanDecided(%d, '%s');",
            it->second, js_escape(label)));
    }

    if (plan_id == wxString::FromUTF8(current_plan_id_)) {
        if (plan_chip_) {
            plan_chip_->SetLabel(
                wxString::Format("Plan: done (%d/%d%s)",
                                  current_plan_done_steps_,
                                  current_plan_total_steps_,
                                  success ? "" : " - with failures"));
            Layout();
        }
    }
}

void ChatPanel::on_error(const wxString& message)
{
    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-error', %s);",
        message_id_, "'" + js_escape(message) + "'"));
}

void ChatPanel::on_auto_commit(const wxString& short_sha,
                               const wxString& branch,
                               const wxString& subject)
{
    if (!commit_chip_) return;
    if (short_sha.empty()) return;

    wxString label = "Commit: " + short_sha;
    if (!branch.empty())
        label += " (" + branch + ")";
    commit_chip_->SetLabel(label);

    wxString tip = subject;
    if (!branch.empty())
        tip = branch + " " + short_sha + ": " + subject;
    commit_chip_->SetToolTip(tip);

    commit_chip_->Show();
    Layout();
}

void ChatPanel::on_tool_pending(const wxString& call_id,
                                const wxString& tool_name,
                                const wxString& preview)
{
    ++message_id_;
    // Remember which DOM message id belongs to this tool call so the eventual
    // on_tool_result -- which can arrive after other unrelated chat events
    // (errors, reasoning blocks, even other tool calls) -- can attach its
    // <details> to the *matching* node, not the latest one.
    tool_call_msg_ids_[std::string(call_id.utf8_string())] = message_id_;

    wxString content = "<span class=\"tool-name\">" + js_escape(tool_name) + "</span>";
    if (!preview.empty())
        content += "<br><span class=\"tool-preview\">" + js_escape(preview) + "</span>";

    run_script(wxString::Format(
        "addMsg(%d, 'msg-tool', %s);",
        message_id_, "'" + js_escape(content) + "'"));
}

void ChatPanel::on_tool_result(const wxString& call_id, const wxString& display)
{
    if (display.empty()) return;

    // Resolve the matching tool-pending message id. If we never saw the
    // matching pending (shouldn't happen, but defend against it), fall back
    // to "latest message" so the result still surfaces somewhere visible.
    int target_id = message_id_;
    auto it = tool_call_msg_ids_.find(std::string(call_id.utf8_string()));
    if (it != tool_call_msg_ids_.end()) {
        target_id = it->second;
        tool_call_msg_ids_.erase(it);
    }

    // Truncate long results for display.
    wxString truncated = display;
    if (truncated.length() > 500)
        truncated = truncated.Left(500) + "... (" +
                    wxString::Format("%zu", display.length() - 500) + " chars truncated)";

    // Append result to the matching tool message as a collapsible <details>.
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
        target_id, "'" + js_escape(truncated) + "'"));
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

void ChatPanel::set_attached_chip(const wxString& file_path)
{
    if (file_path.empty()) {
        if (attach_panel_->IsShown()) {
            attach_panel_->Hide();
            Layout();
        }
        attach_label_->SetLabel("");
        return;
    }
    attach_label_->SetLabel(wxString("Attached: ") + file_path);
    attach_label_->SetToolTip(file_path);
    if (!attach_panel_->IsShown()) {
        attach_panel_->Show();
        Layout();
    }
}

void ChatPanel::set_on_detach(std::function<void()> cb)
{
    on_detach_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Timer: flush tokens to WebView
// ---------------------------------------------------------------------------

void ChatPanel::on_flush_timer(wxTimerEvent& /*evt*/)
{
    // While we're still waiting for the first token, tick the elapsed-seconds
    // counter inside the placeholder once per second. Returns early so the
    // token-flush path below is skipped (buffers are empty anyway).
    if (waiting_for_first_token_) {
        ++wait_ticks_;
        // 33 ms timer interval: every 30 ticks ~= every 1 s.
        if (wait_ticks_ % 30 == 1) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - turn_start_time_).count();
            run_script(wxString::Format(
                "setMsgHtml(%d, '<em style=\"color:#888\">"
                "Thinking... (%llds)</em>');",
                assistant_id_, static_cast<long long>(elapsed)));
        }
        return;
    }

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
    const int key = evt.GetKeyCode();

    // Route navigation keys to the slash popup while it is visible.
    if (slash_popup_visible()) {
        switch (key) {
        case WXK_ESCAPE:
            hide_slash_popup();
            return;
        case WXK_UP:
            slash_popup_->move_up();
            return;
        case WXK_DOWN:
            slash_popup_->move_down();
            return;
        case WXK_TAB: {
            auto sel = slash_popup_->selected_command();
            if (!sel.empty()) {
                accept_slash_suggestion(sel);
                return;
            }
            break;
        }
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
            if (!evt.ShiftDown()) {
                auto sel = slash_popup_->selected_command();
                if (!sel.empty()) {
                    accept_slash_suggestion(sel);
                    return;
                }
            }
            break;
        default:
            break;
        }
    }

    if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && !evt.ShiftDown()) {
        submit_current_input();
    } else {
        evt.Skip();
    }
}

void ChatPanel::on_input_text(wxCommandEvent& evt)
{
    update_slash_popup();
    evt.Skip();
}

bool ChatPanel::submit_current_input()
{
    wxString text = input_->GetValue().Trim().Trim(false);
    if (text.empty()) return false;

    // If this is a known GUI slash command, dispatch it locally.
    if (on_slash_command_ && !text.empty() && text[0] == '/') {
        wxString body = text.Mid(1);  // drop leading '/'
        // Split on first whitespace: name + rest.
        auto is_ws = [](wxUniChar c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        };
        size_t sp = 0;
        while (sp < body.size() && !is_ws(body[sp])) ++sp;
        std::string name = body.Left(sp).ToStdString();
        std::string rest;
        if (sp < body.size()) {
            wxString r = body.Mid(sp);
            r.Trim(false);
            rest = r.ToStdString();
        }
        if (!name.empty() && on_slash_command_(name, rest)) {
            input_->Clear();
            hide_slash_popup();
            return true;
        }
    }

    input_->Clear();
    hide_slash_popup();

    // Add user message bubble.
    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-user', %s);",
        message_id_, "'" + js_escape(text) + "'"));

    // Block typing while agent is working, but keep the dark styling.
    // (Enable/Disable on Windows resets colours to the light system defaults.)
    input_->SetEditable(false);

    if (on_send_)
        on_send_(text.ToStdString(wxConvUTF8));
    return true;
}

// -- Slash popup -----------------------------------------------------------

void ChatPanel::set_slash_commands(std::vector<SlashItem> items)
{
    slash_commands_ = std::move(items);
    // Drop any existing popup; rebuild lazily on next '/'.
    if (slash_popup_) {
        if (slash_popup_shown_) slash_popup_->Dismiss();
        slash_popup_.reset();
        slash_popup_shown_ = false;
    }
}

void ChatPanel::set_on_slash_command(
    std::function<bool(const std::string&, const std::string&)> cb)
{
    on_slash_command_ = std::move(cb);
}

void ChatPanel::append_system_note(const wxString& html)
{
    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-tool', %s);",
        message_id_, "'" + js_escape(html) + "'"));
}

wxString ChatPanel::active_slash_token() const
{
    // We trigger only when the input value starts with '/' and no whitespace
    // has been typed after it yet. Simple, matches "/" as a command-mode
    // indicator for the whole line.
    wxString v = input_->GetValue();
    if (v.empty() || v[0] != '/') return wxEmptyString;

    // If the cursor has moved past a whitespace character, the user is
    // filling in arguments — suggestions should be hidden.
    wxString token;
    for (size_t i = 1; i < v.size(); ++i) {
        wxUniChar c = v[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            return wxString("\x01");  // sentinel: stop suggesting
        token += c;
    }
    return token;
}

bool ChatPanel::slash_popup_visible() const
{
    return slash_popup_ && slash_popup_shown_;
}

void ChatPanel::hide_slash_popup()
{
    if (slash_popup_ && slash_popup_shown_) {
        slash_popup_shown_ = false;
        slash_popup_->Dismiss();
    }
}

void ChatPanel::update_slash_popup()
{
    if (slash_commands_.empty()) return;

    wxString token = active_slash_token();
    if (token.empty() || token == "\x01") {
        hide_slash_popup();
        return;
    }

    // Lazy-construct the popup on first use.
    if (!slash_popup_) {
        slash_popup_ = std::make_unique<SlashPopup>(this, slash_commands_);
        slash_popup_->on_accept = [this](const std::string& name) {
            accept_slash_suggestion(name);
        };
        slash_popup_->on_dismiss = [this]() {
            slash_popup_shown_ = false;
        };
    }

    bool any = slash_popup_->apply_filter(token);
    if (!any) {
        hide_slash_popup();
        return;
    }

    // Anchor just above the input's top edge so the popup overlays the chat
    // history (the input sits at the bottom of the window).
    wxPoint anchor = input_->GetScreenPosition();
    int w = input_->GetSize().GetWidth();
    if (!slash_popup_shown_) {
        slash_popup_->show_anchored(anchor, w);
        slash_popup_shown_ = true;
        input_->SetFocus();
    } else {
        // Already shown — reposition (size may have changed with filter).
        slash_popup_->show_anchored(anchor, w);
    }
}

void ChatPanel::accept_slash_suggestion(const std::string& cmd_name)
{
    if (cmd_name.empty()) return;
    // Replace the input with the full command + trailing space, ready for args.
    wxString new_text = "/" + wxString::FromUTF8(cmd_name) + " ";
    input_->ChangeValue(new_text);  // ChangeValue does not fire wxEVT_TEXT
    input_->SetInsertionPointEnd();
    hide_slash_popup();
    input_->SetFocus();
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

    // S4.D: intercept the locus:// scheme used by plan-bubble Approve / Reject
    // links. URL shape: locus://plan-approve/<plan-id> or locus://plan-reject/<plan-id>.
    // Always veto -- we just dispatch to the C++ side and the page stays put.
    if (url.StartsWith("locus://plan-approve")) {
        evt.Veto();
        // Lock the bubble visually so the user can't double-click. The
        // canonical state update arrives via on_plan_completed / mode change.
        if (!current_plan_id_.empty()) {
            auto it = plan_msg_ids_.find(current_plan_id_);
            if (it != plan_msg_ids_.end()) {
                run_script(wxString::Format(
                    "setPlanDecided(%d, '%s');",
                    it->second, js_escape("Approved -- executing...")));
            }
        }
        if (on_plan_decision_) on_plan_decision_("approve");
        return;
    }
    if (url.StartsWith("locus://plan-reject")) {
        evt.Veto();
        if (!current_plan_id_.empty()) {
            auto it = plan_msg_ids_.find(current_plan_id_);
            if (it != plan_msg_ids_.end()) {
                run_script(wxString::Format(
                    "setPlanDecided(%d, '%s');",
                    it->second, js_escape("Rejected.")));
            }
        }
        if (on_plan_decision_) on_plan_decision_("reject");
        return;
    }

    // Block all other external navigation (user clicking links in chat).
    spdlog::trace("WebView navigation blocked: {}", url.ToStdString());
    evt.Veto();
}

} // namespace locus
