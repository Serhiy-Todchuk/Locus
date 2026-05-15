#include "chat_panel.h"
#include "diff_renderer.h"
#include "locus_accessible.h"
#include "markdown.h"
#include "theme.h"
#include "ui_names.h"

#include "../../agent/mention_parser.h"

#include <spdlog/spdlog.h>

#include <wx/webview.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

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
/* S5.C -- inline tool-result diffs. Bypass md4c entirely (the chat panel
   injects the diff HTML via innerHTML on .tool-diff-wrap, the <div> spans
   below are emitted by render_*_diff_html). */
.tool-diff-wrap {
    margin-top: 6px;
}
.tool-diff {
    border: 1px solid #d8dce0;
    border-radius: 4px;
    overflow: hidden;
    font-family: "Cascadia Code", "Consolas", monospace;
    font-size: 12px;
    background: #ffffff;
}
.tool-diff .diff-file-header {
    background: #eef2f6;
    color: #444;
    padding: 4px 8px;
    border-bottom: 1px solid #d8dce0;
}
.tool-diff .diff-file-header code {
    background: transparent;
    color: #1a2733;
}
.tool-diff .diff-hunk-header {
    background: #f5f7fa;
    color: #6b7785;
    padding: 2px 8px;
    border-top: 1px solid #e6eaef;
    border-bottom: 1px solid #e6eaef;
}
.tool-diff .diff-line {
    white-space: pre-wrap;
    padding: 0;
    margin: 0;
    line-height: 1.4;
    display: flex;
}
.tool-diff .diff-ln {
    flex: 0 0 auto;
    padding: 0 6px;
    color: #8895a6;
    background: #f0f3f6;
    border-right: 1px solid #e1e7ed;
    text-align: right;
    user-select: none;
    white-space: pre;
}
.tool-diff .diff-text {
    flex: 1 1 auto;
    padding: 0 8px;
    white-space: pre-wrap;
}
.tool-diff .diff-line.add { background: #e6ffec; color: #1a7f37; }
.tool-diff .diff-line.add .diff-text::before {
    content: "+ "; color: #1a7f37;
}
.tool-diff .diff-line.del { background: #ffebe9; color: #b91c1c; }
.tool-diff .diff-line.del .diff-text::before {
    content: "- "; color: #b91c1c;
}
.tool-diff .diff-line.ctx { color: #6b7785; }
.tool-diff .diff-line.ctx .diff-text::before { content: "  "; }
.tool-diff .diff-truncated {
    padding: 4px 8px;
    color: #6b7785;
    font-style: italic;
    border-top: 1px solid #e6eaef;
    background: #f5f7fa;
}
.tool-diff .diff-collapsed > summary {
    cursor: pointer;
    user-select: none;
    padding: 4px 8px;
    color: #4a6fa5;
    background: #eef2f6;
    border-top: 1px solid #e6eaef;
    font-style: italic;
}
.tool-diff .diff-collapsed > summary:hover { color: #2952a3; }
.tool-diff .diff-collapsed[open] > summary { border-bottom: 1px solid #e6eaef; }
.tool-diff.diff-deleted .diff-file-header {
    background: #ffebe9;
    color: #b91c1c;
}
.tool-diff .diff-meta {
    color: #6b7785;
    font-weight: normal;
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
    /* S5.C dark-mode diff palette. Tints derived from GitHub Dark + a few
       point tweaks for readability against the chat panel's bg (#252830). */
    .tool-diff {
        background: #1e2230;
        border-color: #3a3f4b;
    }
    .tool-diff .diff-file-header {
        background: #2a3142; color: #d4d4d4;
        border-bottom-color: #3a3f4b;
    }
    .tool-diff .diff-file-header code { color: #ffffff; }
    .tool-diff .diff-hunk-header {
        background: #232938; color: #9ca3af;
        border-top-color: #3a3f4b;
        border-bottom-color: #3a3f4b;
    }
    .tool-diff .diff-line.add { background: #133723; color: #6fdc8c; }
    .tool-diff .diff-line.add .diff-text::before { color: #6fdc8c; }
    .tool-diff .diff-line.del { background: #3b1c1c; color: #f48771; }
    .tool-diff .diff-line.del .diff-text::before { color: #f48771; }
    .tool-diff .diff-line.ctx { color: #888; }
    .tool-diff .diff-ln {
        background: #232938;
        color: #6b7785;
        border-right-color: #3a3f4b;
    }
    .tool-diff .diff-truncated {
        background: #232938; color: #9ca3af;
        border-top-color: #3a3f4b;
    }
    .tool-diff .diff-collapsed > summary {
        background: #232938;
        color: #8aa9d6;
        border-top-color: #3a3f4b;
    }
    .tool-diff .diff-collapsed > summary:hover { color: #b7cbe6; }
    .tool-diff .diff-collapsed[open] > summary {
        border-bottom-color: #3a3f4b;
    }
    .tool-diff.diff-deleted .diff-file-header {
        background: #3b1c1c; color: #f48771;
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
)html";
    // Split here to keep each raw-string literal under MSVC's 16380-byte
    // single-literal cap. The total page is built by concatenation.
    html += R"html(<script>
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
    // S5.L -- name the panel for UI Automation. Children get named in their
    // respective create_*() helpers below.
    SetName(ui_names::kChatPanel);
    gui::apply_locus_accessible_name(this);

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
    mode_chat_btn_->SetName(ui_names::kChatModeChat);
    gui::apply_locus_accessible_name(mode_chat_btn_);
    mode_plan_btn_ = mk_toggle("Plan", AgentMode::plan,
        "Plan mode: model proposes a structured plan; you Approve to execute.");
    mode_plan_btn_->SetName(ui_names::kChatModePlan);
    gui::apply_locus_accessible_name(mode_plan_btn_);
    mode_execute_btn_ = mk_toggle("Execute", AgentMode::execute,
        "Execute mode: full tool catalog plus mark_step_done. "
        "Usually entered automatically after Approve.");
    mode_execute_btn_->SetName(ui_names::kChatModeExec);
    gui::apply_locus_accessible_name(mode_execute_btn_);
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

    // Footer bar. Layout (left to right):
    //   [gauge] [ctx label] [chips...]  ......stretch......  [buttons]
    // ctx_label_ gets growth priority (proportion 1) so the p:/g: split
    // stays visible as the window narrows; the buttons live on the right
    // edge where they're easy to click without competing for the label's
    // horizontal real estate.
    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    footer->Add(ctx_gauge_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(ctx_label_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    footer->Add(plan_chip_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    footer->Add(commit_chip_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    footer->AddStretchSpacer();
    footer->Add(compact_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(undo_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(stop_btn_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(footer, 0, wxEXPAND | wxALL, 4);

    SetSizer(sizer);

    // Bind timer.
    Bind(wxEVT_TIMER, &ChatPanel::on_flush_timer, this);
}

void ChatPanel::create_webview()
{
    webview_ = wxWebView::New(this, wxID_ANY);
    webview_->SetName(ui_names::kChatWebView);
    gui::apply_locus_accessible_name(webview_);
    webview_->SetPage(wxString::FromUTF8(build_chat_html()), "about:blank");

    // Block navigation to external URLs.
    webview_->Bind(wxEVT_WEBVIEW_NAVIGATING, &ChatPanel::on_webview_navigating, this);

    // SetPage() is async in WebView2 — wait for loaded event before running JS.
    webview_->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent&) {
        if (page_ready_) return;  // only handle the first load
        page_ready_ = true;
        // Same reentrancy concern as run_script: each RunScript below yields
        // and can deliver wxThreadEvents that call run_script again. Set the
        // guard so those nested calls land in pending_scripts_, then drain
        // FIFO until empty.
        in_run_script_ = true;
        size_t n = 0;
        while (!pending_scripts_.empty()) {
            wxString next = pending_scripts_.front();
            pending_scripts_.erase(pending_scripts_.begin());
            webview_->RunScript(next);
            ++n;
        }
        in_run_script_ = false;
        spdlog::info("WebView page loaded, flushed {} queued scripts", n);
    });
}

void ChatPanel::create_input()
{
    input_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                            wxDefaultPosition, wxSize(-1, 60),
                            wxTE_MULTILINE | wxTE_PROCESS_ENTER | wxTE_RICH2);
    input_->SetName(ui_names::kChatInput);
    gui::apply_locus_accessible_name(input_);
    // Default Windows RichEdit cap is ~64K UTF-16 chars (and ~32K on plain
    // multiline edits); paste a chunky stack trace or a long file body and
    // the trailing bytes silently vanish. 0 = "no limit" by wx's convention,
    // which on Win32 calls EM_EXLIMITTEXT with -1 -- ample for any single
    // turn, and the agent's context-budget machinery handles the downstream
    // limits properly.
    input_->SetMaxLength(0);
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
    ctx_label_->SetName(ui_names::kChatCtxLabel);
    gui::apply_locus_accessible_name(ctx_label_);
    compact_btn_ = new wxButton(this, wxID_ANY, "Compact",
                                wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    compact_btn_->SetName(ui_names::kChatCompactBtn);
    gui::apply_locus_accessible_name(compact_btn_);
    compact_btn_->SetToolTip("Open context compaction dialog");
    compact_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_compact_) on_compact_();
    });
    stop_btn_ = new wxButton(this, wxID_ANY, "Stop",
                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    stop_btn_->SetName(ui_names::kChatStopBtn);
    gui::apply_locus_accessible_name(stop_btn_);
    stop_btn_->SetToolTip("Stop the current generation");
    stop_btn_->Disable();
    stop_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_stop_) on_stop_();
    });
    undo_btn_ = new wxButton(this, wxID_ANY, "Undo",
                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    undo_btn_->SetName(ui_names::kChatUndoBtn);
    gui::apply_locus_accessible_name(undo_btn_);
    undo_btn_->SetToolTip("Revert files mutated by the most recent turn");
    undo_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_undo_) on_undo_();
    });
    plan_chip_  = new wxStaticText(this, wxID_ANY, "");
    plan_chip_->Hide();  // shown only while a plan is active
    commit_chip_ = new wxStaticText(this, wxID_ANY, "");
    commit_chip_->Hide(); // shown only when git auto-commit is on AND fired
    // S4.F live generation chip rolled into the ctx label (M5 polish) --
    // see refresh_ctx_label / set_generation_progress.
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
    // S5.Z #1 -- after a tool call sealed the previous assistant bubble,
    // assistant_id_ is 0; the next streamed token needs a fresh bubble so
    // post-tool text lands chronologically AFTER the tool bubble instead of
    // being re-rendered into the original (now stale) one.
    if (assistant_id_ == 0) {
        ++message_id_;
        assistant_id_ = message_id_;
        run_script(wxString::Format(
            "addMsg(%d, 'msg-assistant streaming-cursor', '');",
            assistant_id_));
    }
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
    // S5.Z #1 -- mirror the on_token seal-restart: the reasoning <details>
    // block is anchored to the assistant bubble (addReasoning inserts before
    // it), so when assistant_id_ is 0 we need a fresh anchor before queuing
    // the reasoning token. Same idea as on_token: keep the chronological
    // order intact across tool boundaries.
    if (assistant_id_ == 0) {
        ++message_id_;
        assistant_id_ = message_id_;
        run_script(wxString::Format(
            "addMsg(%d, 'msg-assistant streaming-cursor', '');",
            assistant_id_));
    }
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

    // S5.Z #1 -- assistant_id_ may legitimately be 0 here when the turn ends
    // immediately after a tool call (the seal-and-restart path in
    // on_tool_pending zeroed it and no further text token arrived to allocate
    // a fresh bubble). Skip the assistant render in that case; otherwise do
    // the final md4c pass + drop the streaming cursor.
    //
    // Same whitespace-only check as on_tool_pending: a reasoning-only turn
    // (LLM emitted <think>...</think> followed by a stop, no visible text)
    // leaves the assistant bubble allocated by on_reasoning_token but with
    // no real content. Drop it rather than rendering an empty bubble.
    if (assistant_id_ != 0) {
        auto visibly_empty = [](const std::string& s) {
            for (char c : s) {
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    return false;
            }
            return true;
        };
        if (visibly_empty(current_response_)) {
            run_script(wxString::Format(
                "var d=document.getElementById('msg-%d');if(d)d.remove();",
                assistant_id_));
        } else {
            std::string html = markdown_to_html(current_response_);
            run_script(wxString::Format(
                "setMsgHtml(%d, %s);",
                assistant_id_, "'" + js_escape(wxString::FromUTF8(html)) + "'"));
            run_script(wxString::Format(
                "removeClassFromMsg(%d, 'streaming-cursor');", assistant_id_));
        }
    }

    // Highlight code blocks.
    run_script("highlightAll();");

    streaming_ = false;
    stop_btn_->Disable();
    if (undo_btn_) undo_btn_->Enable();
    input_->SetFocus();
    // M5 polish -- the live estimate retired in favour of the ctx label's
    // own "out:~N" mid-stream / "out:N" post-stream rendering. Clear it so
    // the next set_context_meter that fires post-turn (with exact
    // completion_tokens) wins the render.
    live_completion_estimate_ = 0;
    refresh_ctx_label();
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
                                const wxString& preview,
                                const nlohmann::json& args)
{
    // S5.C -- cache args + tool name so on_tool_result can branch into
    // diff rendering without the args being re-threaded.
    {
        PendingToolInfo info;
        info.tool_name = std::string(tool_name.utf8_string());
        info.args      = args;
        pending_tool_info_[std::string(call_id.utf8_string())] = std::move(info);
    }

    // S5.Z #1 -- seal-and-restart. A tool call ends the current assistant
    // streaming run; finalize the open assistant bubble (and reasoning block)
    // here so post-tool tokens open a NEW bubble below the tool, preserving
    // chronological order. Without this the original bubble remained the
    // streaming target and post-tool text visually appeared above the tool.
    if (reasoning_id_ != 0) {
        if (!reasoning_buffer_.empty()) {
            current_reasoning_ += reasoning_buffer_;
            reasoning_buffer_.clear();
            run_script(wxString::Format(
                "setReasoningBody(%d, %s);",
                reasoning_id_,
                "'" + js_escape(wxString::FromUTF8(current_reasoning_)) + "'"));
        }
        run_script(wxString::Format(
            "finalizeReasoning(%d, 'Thoughts');", reasoning_id_));
        reasoning_id_ = 0;
        current_reasoning_.clear();
    }
    if (assistant_id_ != 0) {
        // "Visibly empty" = current_response_ + token_buffer_ contains only
        // whitespace. Models routinely emit a stray newline / space between
        // a reasoning block and a tool call; that token would otherwise pin
        // the bubble in the DOM (literal `empty()` returns false) and the
        // user sees a tiny phantom assistant message under the reasoning
        // "Thoughts" disclosure.
        auto visibly_empty = [](const std::string& s) {
            for (char c : s) {
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    return false;
            }
            return true;
        };
        if (visibly_empty(current_response_) && visibly_empty(token_buffer_)) {
            // No content tokens streamed into this bubble -- it's either still
            // showing the "Thinking..." placeholder or was emptied when only
            // reasoning streamed. Drop the empty bubble entirely so the tool
            // call doesn't sit below a phantom assistant message.
            run_script(wxString::Format(
                "var d=document.getElementById('msg-%d');if(d)d.remove();",
                assistant_id_));
        } else {
            // Flush any remaining buffered tokens and final-render with md4c
            // so the sealed bubble matches the post-turn shape.
            if (!token_buffer_.empty()) {
                current_response_ += token_buffer_;
                token_buffer_.clear();
            }
            std::string html = markdown_to_html(current_response_);
            run_script(wxString::Format(
                "setMsgHtml(%d, %s);",
                assistant_id_,
                "'" + js_escape(wxString::FromUTF8(html)) + "'"));
            run_script(wxString::Format(
                "removeClassFromMsg(%d, 'streaming-cursor');", assistant_id_));
        }
        assistant_id_ = 0;
        current_response_.clear();
        token_buffer_.clear();
    }
    // Placeholder no longer applies once we hand off to a tool; the next
    // text token (if any) opens a fresh bubble without the placeholder.
    waiting_for_first_token_ = false;

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

void ChatPanel::on_tool_result(const wxString& call_id,
                               const wxString& display,
                               bool success)
{
    const std::string call_id_str(call_id.utf8_string());

    // Pop the cached pending info for this call regardless of which branch
    // we take below -- the entry must not outlive the call/result pair.
    std::optional<PendingToolInfo> pending;
    if (auto it = pending_tool_info_.find(call_id_str);
        it != pending_tool_info_.end()) {
        pending = std::move(it->second);
        pending_tool_info_.erase(it);
    }

    if (display.empty() && !success) return;
    if (display.empty() && !pending.has_value()) return;

    // Resolve the matching tool-pending message id. If we never saw the
    // matching pending (shouldn't happen, but defend against it), fall back
    // to "latest message" so the result still surfaces somewhere visible.
    int target_id = message_id_;
    auto it = tool_call_msg_ids_.find(call_id_str);
    if (it != tool_call_msg_ids_.end()) {
        target_id = it->second;
        tool_call_msg_ids_.erase(it);
    }

    // S5.C -- inline diff render for successful edit_file / write_file /
    // delete_file. Bypass md4c entirely; the diff HTML is constructed from
    // pre-escaped fragments and injected via innerHTML so the line spans
    // survive intact.
    if (success && diff_show_ && pending.has_value()) {
        const std::string& tool_name = pending->tool_name;
        const auto&        args      = pending->args;

        std::string diff_html;
        DiffRenderOptions opts;
        opts.max_lines          = diff_max_lines_;
        opts.context_lines      = diff_context_lines_;
        opts.collapse_threshold = diff_collapse_threshold_;

        if (tool_name == "edit_file") {
            const std::string path = args.value("path", std::string{});
            std::optional<std::string> old_content;
            if (pre_mutation_fetcher_ && !path.empty()) {
                old_content = pre_mutation_fetcher_(path);
            }
            diff_html = render_edit_file_diff_html(args, old_content, opts);
        } else if (tool_name == "write_file") {
            const std::string path = args.value("path", std::string{});
            std::string new_content = args.value("content", std::string{});
            std::optional<std::string> old_content;
            if (pre_mutation_fetcher_ && !path.empty()) {
                old_content = pre_mutation_fetcher_(path);
            }
            diff_html = render_write_file_diff_html(path, old_content,
                                                    new_content, opts);
        } else if (tool_name == "delete_file") {
            const std::string path = args.value("path", std::string{});
            int line_count = 0;
            if (pre_mutation_fetcher_ && !path.empty()) {
                auto old_content = pre_mutation_fetcher_(path);
                if (old_content.has_value()) {
                    // Count \n + 1 for trailing-line-without-newline case.
                    line_count = 0;
                    for (char c : *old_content) if (c == '\n') ++line_count;
                    if (!old_content->empty() && old_content->back() != '\n')
                        ++line_count;
                }
            }
            diff_html = render_delete_file_summary_html(path, line_count);
        }

        if (!diff_html.empty()) {
            run_script(wxString::Format(
                "var d=document.getElementById('msg-%d');"
                "if(d){var w=document.createElement('div');"
                "w.className='tool-diff-wrap';"
                "w.innerHTML=%s;"
                "d.appendChild(w);"
                "window.scrollTo(0,document.body.scrollHeight);}",
                target_id, "'" + js_escape(wxString::FromUTF8(diff_html)) + "'"));
            // For mutating tools the diff IS the result; suppress the
            // verbose collapsible result block (its text usually just
            // says "wrote N bytes" anyway). Failures still need the
            // result text so the user sees the error.
            if (tool_name == "edit_file"  ||
                tool_name == "write_file" ||
                tool_name == "delete_file") {
                return;
            }
        }
    }

    if (display.empty()) return;

    // Truncate long results for display.
    wxString truncated = display;
    if (truncated.length() > 500)
        truncated = truncated.Left(500) + "... (" +
                    wxString::Format("%zu", display.length() - 500) + " chars truncated)";

    // Append result to the matching tool message as a collapsible <details>.
    run_script(wxString::Format(
        "var d=document.getElementById('msg-%d');"
        "if(d){var det=document.createElement('details');"
        "det.className='tool-result-details%s';"
        "var sum=document.createElement('summary');"
        "sum.textContent=%s;"
        "det.appendChild(sum);"
        "var pre=document.createElement('pre');"
        "pre.className='tool-result';pre.textContent=%s;"
        "det.appendChild(pre);"
        "d.appendChild(det);"
        "window.scrollTo(0,document.body.scrollHeight);}",
        target_id,
        success ? "" : " tool-result-error",
        success ? "'Result'" : "'Error'",
        "'" + js_escape(truncated) + "'"));
}

void ChatPanel::set_context_meter(int used, int limit,
                                   int prompt_tokens, int completion_tokens)
{
    last_ctx_used_       = used;
    last_ctx_limit_      = limit;
    last_ctx_prompt_     = prompt_tokens;
    last_ctx_completion_ = completion_tokens;
    // The post-round broadcast carries the exact completion_tokens; the
    // live estimate is no longer meaningful so reset it.
    if (completion_tokens > 0) live_completion_estimate_ = 0;
    refresh_ctx_label();
}

void ChatPanel::refresh_ctx_label()
{
    int used  = last_ctx_used_;
    int limit = last_ctx_limit_;
    int pct = (limit > 0) ? (used * 100 / limit) : 0;
    if (ctx_gauge_) ctx_gauge_->SetValue(std::min(pct, 100));

    // S4.V Task 8 + M5 polish -- compose "ctx: U/L (P%, in:P out:Out)".
    // `out:` uses the exact server-reported value when we have it, falls
    // back to the live estimate (prefixed `~`) while a stream is in
    // flight, and is omitted entirely when neither is known (e.g. fresh
    // session before the first LLM round).
    wxString out_part;
    if (last_ctx_completion_ > 0) {
        out_part = wxString::Format(" out:%d", last_ctx_completion_);
    } else if (live_completion_estimate_ > 0) {
        out_part = wxString::Format(" out:~%d", live_completion_estimate_);
    }
    wxString in_part;
    if (last_ctx_prompt_ > 0) {
        in_part = wxString::Format(" in:%d", last_ctx_prompt_);
    }

    wxString label;
    if (!in_part.IsEmpty() || !out_part.IsEmpty()) {
        label = wxString::Format("ctx: %d/%d (%d%%,%s%s)",
                                 used, limit, pct,
                                 in_part.c_str(), out_part.c_str());
    } else {
        label = wxString::Format("ctx: %d/%d (%d%%)", used, limit, pct);
    }
    if (ctx_label_) ctx_label_->SetLabel(label);

    // Color: green < 60%, yellow 60-80%, red > 80%.
    if (ctx_gauge_) {
        if (pct < 60)
            ctx_gauge_->SetForegroundColour(wxColour(76, 175, 80));
        else if (pct < 80)
            ctx_gauge_->SetForegroundColour(wxColour(255, 193, 7));
        else
            ctx_gauge_->SetForegroundColour(wxColour(244, 67, 54));
    }
}

void ChatPanel::set_generation_progress(int /*chars*/, int est_tokens)
{
    // M5 polish -- live estimate now rides in the ctx label as "out:~N"
    // alongside the in: split, so the user has one place to read the
    // turn's token state instead of two competing widgets.
    live_completion_estimate_ = std::max(0, est_tokens);
    refresh_ctx_label();
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
    // Reentrancy guard. wxWebViewEdge::RunScript calls wxYield internally;
    // a slow flush body that yields longer than the 33 ms timer interval
    // would otherwise let the next timer tick re-enter this handler and
    // recurse until the stack overflows (observed Win11 + dark-mode menu
    // message dispatch amplifies the per-level frame cost). Buffers are
    // not cleared on the bail-out, so the next safe tick picks them up.
    if (in_flush_timer_) return;
    struct Guard {
        bool& flag;
        Guard(bool& f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    } guard(in_flush_timer_);

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

    // S4.V -- same nav surface for the @-mention popup.
    if (mention_popup_visible()) {
        switch (key) {
        case WXK_ESCAPE:
            hide_mention_popup();
            return;
        case WXK_UP:
            mention_popup_->move_up();
            return;
        case WXK_DOWN:
            mention_popup_->move_down();
            return;
        case WXK_TAB: {
            auto sel = mention_popup_->selected_path();
            if (!sel.empty()) {
                accept_mention_suggestion(sel);
                return;
            }
            break;
        }
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
            if (!evt.ShiftDown()) {
                auto sel = mention_popup_->selected_path();
                if (!sel.empty()) {
                    accept_mention_suggestion(sel);
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
    update_mention_popup();
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
    hide_mention_popup();

    // S4.V -- pull the first valid `@<path>` mention out of the message and
    // auto-attach it. The token stays in the message verbatim so the user
    // can see what they pinned and the LLM has the reference inline.
    // Multi-mention attach lands when the agent core grows a list slot;
    // last-wins matches the existing single-slot model.
    if (on_mention_attach_ && !mention_paths_.empty()) {
        auto std_text = text.ToStdString(wxConvUTF8);
        auto mentions = parse_mentions(std_text);
        std::unordered_set<std::string> known(
            mention_paths_.begin(), mention_paths_.end());
        for (const auto& m : mentions) {
            if (known.find(m.path) != known.end()) {
                on_mention_attach_(m.path);
                break;
            }
        }
    }

    // Add user message bubble. user_text_to_html does the HTML escaping +
    // newline -> <br> conversion (so Shift+Enter renders as a visible line
    // break, not a single space or a box glyph for unusual line-break code
    // points). js_escape then handles JS-string-literal safety only.
    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-user', %s);",
        message_id_, "'" + js_escape(user_text_to_html(text)) + "'"));

    // Input stays editable while the agent is working -- AgentCore::send_message
    // is non-blocking and queues onto the agent thread, so the user can compose
    // the next message and hit Enter; it'll be picked up after the current turn
    // completes. (The previous SetEditable(false) was unnecessary -- there was
    // never a race the input lock prevented.)

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

// -- @-mention popup (S4.V) -------------------------------------------------

void ChatPanel::set_mention_paths(std::vector<std::string> paths)
{
    mention_paths_ = std::move(paths);
    if (mention_popup_) {
        if (mention_popup_shown_) mention_popup_->Dismiss();
        mention_popup_.reset();
        mention_popup_shown_ = false;
    }
}

void ChatPanel::set_on_mention_attach(std::function<void(const std::string&)> cb)
{
    on_mention_attach_ = std::move(cb);
}

ChatPanel::ActiveMention ChatPanel::active_mention_at_cursor() const
{
    ActiveMention out;
    if (!input_) return out;

    long ins = input_->GetInsertionPoint();
    wxString v = input_->GetValue();
    if (ins <= 0 || static_cast<size_t>(ins) > v.size()) return out;

    // Walk back from the cursor to the nearest '@'. Reject the run if a
    // disallowed (path-terminating) char appears before we hit one, or if
    // the char immediately preceding the '@' is alphanumeric / dot / etc.
    // (the same heuristic as parse_mentions, so the popup doesn't fire
    // mid-email).
    auto is_path_char = [](wxUniChar c) {
        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z')) return true;
        switch (static_cast<wchar_t>(c)) {
            case '/': case '\\':
            case '.': case '_': case '-':
            case '+': case '~': case '#':
                return true;
            default: return false;
        }
    };
    auto allowed_before_at = [](wxUniChar c) {
        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z')) return false;
        switch (static_cast<wchar_t>(c)) {
            case '.': case '_': case '-': return false;
            default: return true;
        }
    };

    long i = ins - 1;
    while (i >= 0) {
        wxUniChar c = v[static_cast<size_t>(i)];
        if (c == '@') {
            if (i == 0 || allowed_before_at(v[static_cast<size_t>(i - 1)])) {
                out.start  = static_cast<size_t>(i);
                out.prefix = v.SubString(i + 1, ins - 1);
                return out;
            }
            return out;  // '@' but bad neighbour -- treat as no-mention.
        }
        if (!is_path_char(c)) return out;
        --i;
    }
    return out;
}

bool ChatPanel::mention_popup_visible() const
{
    return mention_popup_ && mention_popup_shown_;
}

void ChatPanel::hide_mention_popup()
{
    if (mention_popup_ && mention_popup_shown_) {
        mention_popup_shown_ = false;
        mention_popup_->Dismiss();
    }
}

void ChatPanel::update_mention_popup()
{
    if (mention_paths_.empty()) return;

    ActiveMention m = active_mention_at_cursor();
    if (m.start == std::string::npos) {
        hide_mention_popup();
        return;
    }

    if (!mention_popup_) {
        mention_popup_ = std::make_unique<MentionPopup>(this, mention_paths_);
        mention_popup_->on_accept = [this](const std::string& path) {
            accept_mention_suggestion(path);
        };
        mention_popup_->on_dismiss = [this]() {
            mention_popup_shown_ = false;
        };
    }

    bool any = mention_popup_->apply_filter(m.prefix);
    if (!any) {
        hide_mention_popup();
        return;
    }

    wxPoint anchor = input_->GetScreenPosition();
    int w = input_->GetSize().GetWidth();
    if (!mention_popup_shown_) {
        mention_popup_->show_anchored(anchor, w);
        mention_popup_shown_ = true;
        input_->SetFocus();
    } else {
        mention_popup_->show_anchored(anchor, w);
    }
}

void ChatPanel::accept_mention_suggestion(const std::string& path)
{
    if (path.empty() || !input_) return;
    ActiveMention m = active_mention_at_cursor();
    if (m.start == std::string::npos) {
        hide_mention_popup();
        return;
    }

    wxString v = input_->GetValue();
    long ins = input_->GetInsertionPoint();
    // Replace [start .. ins) with "@<path> ".
    wxString before = v.SubString(0, static_cast<long>(m.start) - 1);
    wxString after  = v.SubString(ins, v.size() - 1);
    wxString inserted = "@" + wxString::FromUTF8(path) + " ";
    wxString new_text = before + inserted + after;

    input_->ChangeValue(new_text);  // does not fire wxEVT_TEXT
    long new_caret = static_cast<long>(before.size() + inserted.size());
    input_->SetInsertionPoint(new_caret);
    hide_mention_popup();
    input_->SetFocus();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void ChatPanel::run_script(const wxString& js)
{
    if (!webview_) return;

    // Queue when the page hasn't finished loading OR when we're already
    // inside a RunScript on this thread. The reentrancy case is the
    // important one: wxWebViewEdge::RunScript pumps the event loop via
    // wxYield, which delivers queued wxThreadEvents from the agent thread
    // (token / tool / plan callbacks). Those handlers also call
    // run_script -- without queueing, each nested call would invoke
    // another RunScript with its own yield, recursing until the stack
    // overflows. The outermost run_script drains pending_scripts_ FIFO
    // once its own RunScript returns, so call order is preserved.
    if (!page_ready_ || in_run_script_) {
        pending_scripts_.push_back(js);
        return;
    }

    in_run_script_ = true;
    webview_->RunScript(js);
    // Drain anything that arrived during the yield. Each drained call
    // can itself yield and queue more, so loop until empty. Re-check
    // page_ready_ each iteration in case a navigation invalidated it.
    while (page_ready_ && !pending_scripts_.empty()) {
        wxString next = pending_scripts_.front();
        pending_scripts_.erase(pending_scripts_.begin());
        webview_->RunScript(next);
    }
    in_run_script_ = false;
}

wxString ChatPanel::user_text_to_html(const wxString& s)
{
    // Normalise every line-break flavour to a single \n first; then a single
    // escape pass produces one `<br>` per logical line. Without normalisation
    // a CRLF pair would produce `<br><br>` and double the gap.
    wxString norm = s;
    norm.Replace("\r\n", "\n", true);
    norm.Replace("\r",   "\n", true);
    // Windows RichEdit (wxTE_RICH2) inserts U+000B (vertical tab) as a "soft
    // return" on Shift+Enter, not \r\n. Without this it would reach the DOM
    // raw and render as a control-character glyph under white-space: pre-wrap.
    norm.Replace("\v", "\n", true);                                // U+000B VT
    norm.Replace("\f", "\n", true);                                // U+000C FF
    norm.Replace(wxString::FromUTF8("\xC2\x85"),     "\n", true); // U+0085 NEL
    norm.Replace(wxString::FromUTF8("\xE2\x80\xA8"), "\n", true); // U+2028 LS
    norm.Replace(wxString::FromUTF8("\xE2\x80\xA9"), "\n", true); // U+2029 PS

    wxString out;
    out.reserve(norm.length() + 16);
    for (auto ch : norm) {
        switch (ch.GetValue()) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\n': out += "<br>";   break;
        default:   out += ch;       break;
        }
    }
    return out;
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
