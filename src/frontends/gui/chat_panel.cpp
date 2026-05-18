#include "chat_panel.h"
#include "chat/chat_footer_chips.h"
#include "chat/chat_link_handler.h"
#include "chat/chat_popups.h"
#include "chat/chat_stream_renderer.h"
#include "chat/chat_util.h"
#include "diff_renderer.h"
#include "locus_accessible.h"
#include "markdown.h"
#include "theme.h"
#include "ui_names.h"

#include "../../agent/conversation.h"
#include "../../agent/mention_parser.h"
#include "../../llm/token_counter.h"

#include <spdlog/spdlog.h>

#include <wx/stdpaths.h>
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
// Prism.js -- bundled locally under <exe_dir>/resources/prism/ so the chat
// WebView highlights code without reaching for a CDN.  Refresh the bundle via
// third_party/prism/build-bundle.ps1.
// ---------------------------------------------------------------------------

static std::string read_prism_asset(const char* filename)
{
    auto exe = wxStandardPaths::Get().GetExecutablePath().ToStdString();
    auto path = std::filesystem::path(exe).parent_path() /
                "resources" / "prism" / filename;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        spdlog::warn("ChatPanel: Prism asset missing: {}", path.string());
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// Chat HTML template
// ---------------------------------------------------------------------------

std::string ChatPanel::build_chat_html()
{
    std::string html = R"html(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<style media="(prefers-color-scheme: light)">)html";
    html += read_prism_asset("prism.min.css");
    html += R"html(</style>
<style media="(prefers-color-scheme: dark)">)html";
    html += read_prism_asset("prism-tomorrow.min.css");
    html += R"html(</style>
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
/* S5.C -- inline tool-result diffs. */
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

/* S5.D per-message token chip */
.token-chip {
    display: inline-block;
    font-size: 10px;
    color: #aaa;
    margin-left: 6px;
    vertical-align: middle;
    user-select: none;
    opacity: 0.8;
}

/* S5.G per-message delete X (hover-reveal). The .msg container is the
   positioning context; the X uses absolute placement. Opacity transitions
   so the chrome doesn't pop in -- soft reveal feels less aggressive. */
.msg { position: relative; }
.msg-delete-x {
    position: absolute;
    top: 4px;
    right: 6px;
    width: 20px;
    height: 20px;
    line-height: 18px;
    text-align: center;
    background: rgba(0, 0, 0, 0.10);
    color: #6b7785;
    text-decoration: none;
    border-radius: 50%;
    font-size: 14px;
    opacity: 0;
    transition: opacity 0.15s ease-in-out, background 0.15s ease-in-out;
    user-select: none;
}
.msg:hover .msg-delete-x { opacity: 1; }
.msg-delete-x:hover { background: rgba(185, 28, 28, 0.85); color: #fff; }

/* S5.G system-prompt bubble (collapsed by default; expand for per-section
   token breakdown + full prompt text). */
.msg-system-prompt {
    align-self: stretch;
    background: #f5f7fa;
    color: #3a444f;
    border: 1px dashed #c8d0d8;
    border-radius: 8px;
    font-size: 12px;
    max-width: 100%;
    padding: 8px 12px;
}
.msg-system-prompt > summary {
    cursor: pointer;
    color: #4a5664;
    font-weight: 600;
    user-select: none;
    list-style: none;
}
.msg-system-prompt > summary::-webkit-details-marker { display: none; }
.msg-system-prompt > summary::before {
    content: "> ";
    color: #6b7785;
}
.msg-system-prompt[open] > summary::before { content: "v "; }
.msg-system-prompt .sp-breakdown {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin: 8px 0 6px 0;
}
.msg-system-prompt .section-chip {
    display: inline-block;
    padding: 2px 8px;
    background: #e6ecf2;
    color: #4a5664;
    border-radius: 10px;
    font-size: 11px;
}
.msg-system-prompt .sp-body {
    margin-top: 6px;
    padding: 8px;
    background: #ffffff;
    border: 1px solid #d8dce0;
    border-radius: 4px;
    font-family: "Cascadia Code", "Consolas", monospace;
    font-size: 11px;
    white-space: pre-wrap;
    max-height: 320px;
    overflow-y: auto;
    color: #2a3038;
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
    /* S5.C dark-mode diff palette. */
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
    /* S5.G dark-mode palette for delete-X and system-prompt bubble. */
    .msg-delete-x {
        background: rgba(255, 255, 255, 0.08);
        color: #aab1bb;
    }
    .msg-delete-x:hover {
        background: rgba(244, 135, 113, 0.85);
        color: #1e1e1e;
    }
    .msg-system-prompt {
        background: #232a36;
        color: #c8cfd8;
        border-color: #3a4250;
    }
    .msg-system-prompt > summary { color: #b8c0cc; }
    .msg-system-prompt > summary::before { color: #8aa9d6; }
    .msg-system-prompt .section-chip {
        background: #2d3644;
        color: #b8c0cc;
    }
    .msg-system-prompt .sp-body {
        background: #1a1f28;
        color: #c0c6cf;
        border-color: #3a4250;
    }
}
</style>
</head><body>
<div id="chat"></div>
)html";
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
function setMsgTokenChip(id, n) {
    var d = document.getElementById('msg-' + id);
    if (!d) return;
    var chip = d.querySelector('.token-chip');
    if (!chip) { chip = document.createElement('span'); chip.className = 'token-chip'; d.appendChild(chip); }
    chip.textContent = '(' + n + ' t)';
}
/* S5.G -- inject the hover-reveal X for a deletable bubble. msgId is the
   dom message_id_ ChatPanel allocated when creating the bubble; historyId
   is the stable ConversationHistory id the agent core uses for delete. The
   href fires through wxEVT_WEBVIEW_NAVIGATING to ChatLinkHandler. */
function addDeleteButton(msgId, historyId) {
    var d = document.getElementById('msg-' + msgId);
    if (!d) return;
    if (d.querySelector('.msg-delete-x')) return;
    var a = document.createElement('a');
    a.className = 'msg-delete-x';
    a.href = 'locus://delete-message/' + historyId;
    a.textContent = '×';
    a.title = 'Delete this message';
    d.appendChild(a);
}
/* S5.G -- remove a bubble entirely (post-delete). */
function removeMsg(id) {
    var d = document.getElementById('msg-' + id);
    if (d) d.remove();
}
/* S5.G -- collapsed system-prompt bubble at the top of the chat. Renders
   the per-section token breakdown chips + full prompt text inside a
   <details> element so it stays out of the way by default. breakdown is
   { total: N, sections: [{label, tokens}, ...] }. */
function addSystemPromptMsg(id, fullText, breakdown) {
    var existing = document.getElementById('msg-' + id);
    if (existing) existing.remove();
    var d = document.createElement('details');
    d.id = 'msg-' + id;
    d.className = 'msg msg-system-prompt';
    d.open = false;
    var total = (breakdown && breakdown.total) ? breakdown.total : 0;
    var html = '<summary>System prompt (' + total + ' tokens)</summary>';
    html += '<div class="sp-breakdown">';
    var sections = (breakdown && breakdown.sections) ? breakdown.sections : [];
    for (var k = 0; k < sections.length; ++k) {
        var s = sections[k];
        html += '<span class="section-chip">' + escapeHtml(s.label) +
                ': ' + s.tokens + '</span>';
    }
    html += '</div>';
    html += '<pre class="sp-body">' + escapeHtml(fullText) + '</pre>';
    d.innerHTML = html;
    var chat = document.getElementById('chat');
    chat.insertBefore(d, chat.firstChild);
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
    d.innerHTML = '<summary>Thinking...</summary><div class="reasoning-body"></div>';
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
<script>)html";
    html += read_prism_asset("prism.min.js");
    html += R"html(</script>
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
    SetName(ui_names::kChatPanel);
    gui::apply_locus_accessible_name(this);

    create_webview();
    create_input();

    // Footer chips must be created before create_footer() wires the sizer.
    footer_chips_ = std::make_unique<ChatFooterChips>(this);

    create_footer();

    // Collaborators that need the input widget.
    popups_ = std::make_unique<ChatPopups>(this, input_);

    // Streaming renderer shares message_id_ by reference.
    renderer_ = std::make_unique<ChatStreamRenderer>(
        [this](const wxString& js) { run_script(js); },
        message_id_);

    // Link handler for locus:// URL dispatch.
    link_handler_ = std::make_unique<ChatLinkHandler>(
        [this](const wxString& js) { run_script(js); },
        on_plan_decision_,
        // S5.G -- per-message delete dispatch. Prompts the confirm dialog
        // (modal on this panel) and routes the history_id to LocusFrame's
        // on_delete_message_ closure (which calls AgentCore::delete_message).
        [this](int history_id) {
            if (!on_delete_message_) return;
            int answer = wxMessageBox(
                "Delete this message?\nSubsequent turns will no longer see it.",
                "Delete message",
                wxYES_NO | wxICON_QUESTION,
                this);
            if (answer != wxYES) return;
            on_delete_message_(history_id);
        });

    // S4.D mode switcher (Chat / Plan / Execute) -- mutually exclusive toggles.
    auto mk_toggle = [this](const wxString& label, AgentMode m,
                            const wxString& tip) {
        auto* btn = new wxToggleButton(this, wxID_ANY, label,
                                       wxDefaultPosition, wxDefaultSize,
                                       wxBU_EXACTFIT);
        btn->SetToolTip(tip);
        btn->Bind(wxEVT_TOGGLEBUTTON, [this, m, btn](wxCommandEvent&) {
            if (!btn->GetValue()) { btn->SetValue(true); return; }
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
    mode_chat_btn_->SetValue(true);

    // Attached-context chip row.
    attach_panel_ = new wxPanel(this, wxID_ANY);
    attach_label_ = new wxStaticText(attach_panel_, wxID_ANY, "");
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
    attach_panel_->Hide();

    // Mode switcher row.
    auto* mode_row = new wxBoxSizer(wxHORIZONTAL);
    mode_row->Add(new wxStaticText(this, wxID_ANY, "Mode:"),
                  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    mode_row->Add(mode_chat_btn_,    0, wxRIGHT, 2);
    mode_row->Add(mode_plan_btn_,    0, wxRIGHT, 2);
    mode_row->Add(mode_execute_btn_, 0);

    // Footer bar.
    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    footer->Add(footer_chips_->gauge(),       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(footer_chips_->ctx_label(),   1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    footer->Add(footer_chips_->plan_chip(),   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    footer->Add(footer_chips_->commit_chip(), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    footer->AddStretchSpacer();
    footer->Add(compact_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(undo_btn_,    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(stop_btn_,    0, wxALIGN_CENTER_VERTICAL);

    // Main vertical layout.
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(webview_,      1, wxEXPAND);
    sizer->Add(attach_panel_, 0, wxEXPAND | wxTOP | wxBOTTOM, 2);
    sizer->Add(mode_row,      0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 2);
    sizer->Add(input_,        0, wxEXPAND | wxTOP, 2);
    sizer->Add(footer,        0, wxEXPAND | wxALL, 4);
    SetSizer(sizer);

    Bind(wxEVT_TIMER, &ChatPanel::on_flush_timer, this);
}

ChatPanel::~ChatPanel() = default;

void ChatPanel::create_webview()
{
    webview_ = wxWebView::New(this, wxID_ANY);
    webview_->SetName(ui_names::kChatWebView);
    gui::apply_locus_accessible_name(webview_);
    webview_->SetPage(wxString::FromUTF8(build_chat_html()), "about:blank");

    webview_->Bind(wxEVT_WEBVIEW_NAVIGATING, &ChatPanel::on_webview_navigating, this);

    webview_->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent&) {
        if (page_ready_) return;
        page_ready_ = true;
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
    input_->SetMaxLength(0);
    input_->SetToolTip(
        "Type a message and press Enter to send.\n"
        "Shift+Enter inserts a newline. Type '/' for commands.");
    input_->SetBackgroundColour(theme::text_bg());
    input_->SetForegroundColour(theme::text_fg());
    input_->Bind(wxEVT_KEY_DOWN, &ChatPanel::on_input_key, this);
    input_->Bind(wxEVT_TEXT,     &ChatPanel::on_input_text, this);
    input_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& evt) {
        popups_->dismiss_all();
        evt.Skip();
    });
}

void ChatPanel::create_footer()
{
    // ctx_gauge_ and ctx_label_ live in footer_chips_; access via getters.
    // The UIA-accessible name must be set here since the footer chips don't
    // have a reference to ui_names.
    footer_chips_->ctx_label()->SetName(ui_names::kChatCtxLabel);
    gui::apply_locus_accessible_name(footer_chips_->ctx_label());

    compact_btn_ = new wxButton(this, wxID_ANY, "Compact",
                                wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    compact_btn_->SetName(ui_names::kChatCompactBtn);
    gui::apply_locus_accessible_name(compact_btn_);
    compact_btn_->SetToolTip("Open context compaction dialog");
    compact_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_compact_) on_compact_();
    });

    stop_btn_ = new wxButton(this, wxID_ANY, "Submit",
                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    stop_btn_->SetName(ui_names::kChatStopBtn);
    gui::apply_locus_accessible_name(stop_btn_);
    stop_btn_->Disable();
    stop_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (renderer_->is_streaming()) {
            bool has_text = !input_->GetValue().Trim().Trim(false).IsEmpty();
            if (has_text) submit_current_input();
            else if (on_stop_) on_stop_();
        } else {
            submit_current_input();
        }
    });

    undo_btn_ = new wxButton(this, wxID_ANY, "Undo",
                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    undo_btn_->SetName(ui_names::kChatUndoBtn);
    gui::apply_locus_accessible_name(undo_btn_);
    undo_btn_->SetToolTip("Revert files mutated by the most recent turn");
    undo_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_undo_) on_undo_();
    });

    refresh_action_btn();
}

// ---------------------------------------------------------------------------
// Public API -- called from LocusFrame event handlers
// ---------------------------------------------------------------------------

void ChatPanel::on_turn_start()
{
    renderer_->begin_turn();
    refresh_action_btn();
    if (undo_btn_) undo_btn_->Disable();
    flush_timer_.Start(k_flush_interval_ms);
}

void ChatPanel::on_token(const wxString& token)
{
    renderer_->append_token(token);
}

void ChatPanel::on_reasoning_token(const wxString& token)
{
    renderer_->append_reasoning_token(token);
}

void ChatPanel::on_turn_complete()
{
    flush_timer_.Stop();
    renderer_->end_turn();

    // S5.D -- annotate the assistant bubble with server-reported completion
    // tokens once the turn is sealed (end_turn has finalized the HTML).
    if (show_per_message_tokens_ && last_completion_tokens_ > 0) {
        int aid = renderer_->current_assistant_id();
        if (aid > 0)
            run_script(wxString::Format("setMsgTokenChip(%d, %d);",
                                        aid, last_completion_tokens_));
    }

    footer_chips_->clear_live_estimate();
    refresh_action_btn();
    if (undo_btn_) undo_btn_->Enable();
    input_->SetFocus();
}

void ChatPanel::on_session_reset()
{
    run_script("clearChat();");
    renderer_->reset();
    link_handler_->clear_plans();
    if (footer_chips_->hide_plan_chip()) Layout();
    on_mode_changed(AgentMode::chat);
    refresh_action_btn();
    message_id_ = 0;
    tool_call_msg_ids_.clear();
    pending_tool_info_.clear();
    // S5.G -- reset chat-side delete bookkeeping.
    history_to_dom_.clear();
    last_user_dom_id_ = 0;
    pending_tool_history_id_ = 0;
}

// ---------------------------------------------------------------------------
// S4.D plan-mode display
// ---------------------------------------------------------------------------

void ChatPanel::on_mode_changed(AgentMode mode)
{
    if (mode_chat_btn_)    mode_chat_btn_->SetValue(mode == AgentMode::chat);
    if (mode_plan_btn_)    mode_plan_btn_->SetValue(mode == AgentMode::plan);
    if (mode_execute_btn_) mode_execute_btn_->SetValue(mode == AgentMode::execute);

    if (mode == AgentMode::chat) {
        if (footer_chips_->hide_plan_chip()) Layout();
    }
}

void ChatPanel::on_plan_proposed(const wxString& plan_json)
{
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
    link_handler_->register_plan(id, message_id_);
    if (footer_chips_->on_plan_proposed(id, total, first_step)) Layout();

    run_script(wxString::Format("addPlanMsg(%d, '%s');",
                                message_id_, chat_js_escape(plan_json)));
}

void ChatPanel::on_plan_step_advanced(const wxString& plan_id, int step_idx,
                                       const wxString& status,
                                       const wxString& notes)
{
    const std::string id_str(plan_id.utf8_string());
    int msg_id = link_handler_->lookup_msg_id(id_str);
    if (msg_id < 0) {
        spdlog::warn("ChatPanel: step advance for unknown plan id '{}'",
                     id_str);
        return;
    }

    run_script(wxString::Format(
        "updatePlanStep(%d, %d, '%s', '%s');",
        msg_id, step_idx, chat_js_escape(status), chat_js_escape(notes)));

    if (footer_chips_->on_plan_step_advanced(id_str, status)) Layout();
}

void ChatPanel::on_plan_completed(const wxString& plan_id, bool success)
{
    const std::string id_str(plan_id.utf8_string());
    int msg_id = link_handler_->lookup_msg_id(id_str);
    if (msg_id >= 0) {
        wxString label = success ? "Plan completed."
                                  : "Plan completed with failures.";
        run_script(wxString::Format(
            "setPlanDecided(%d, '%s');",
            msg_id, chat_js_escape(label)));
    }
    if (footer_chips_->on_plan_completed(id_str, success)) Layout();
}

void ChatPanel::on_error(const wxString& message)
{
    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-error', %s);",
        message_id_, "'" + chat_js_escape(message) + "'"));
}

void ChatPanel::on_auto_commit(const wxString& short_sha,
                               const wxString& branch,
                               const wxString& subject)
{
    if (footer_chips_->on_auto_commit(short_sha, branch, subject)) Layout();
}

void ChatPanel::on_tool_pending(const wxString& call_id,
                                const wxString& tool_name,
                                const wxString& preview,
                                const nlohmann::json& args)
{
    // Cache args + tool name for diff rendering in on_tool_result.
    {
        PendingToolInfo info;
        info.tool_name = std::string(tool_name.utf8_string());
        info.args      = args;
        pending_tool_info_[std::string(call_id.utf8_string())] = std::move(info);
    }

    // S5.Z #1 -- seal the current streaming bubble before inserting the tool bubble.
    renderer_->seal_bubble();
    // seal_bubble clears waiting_for_first_token_ internally.

    ++message_id_;
    tool_call_msg_ids_[std::string(call_id.utf8_string())] = message_id_;

    wxString content = "<span class=\"tool-name\">" + chat_js_escape(tool_name) + "</span>";
    if (!preview.empty())
        content += "<br><span class=\"tool-preview\">" + chat_js_escape(preview) + "</span>";

    run_script(wxString::Format(
        "addMsg(%d, 'msg-tool', %s);",
        message_id_, "'" + chat_js_escape(content) + "'"));
}

void ChatPanel::on_tool_result(const wxString& call_id,
                               const wxString& display,
                               bool success)
{
    const std::string call_id_str(call_id.utf8_string());

    std::optional<PendingToolInfo> pending;
    if (auto it = pending_tool_info_.find(call_id_str);
        it != pending_tool_info_.end()) {
        pending = std::move(it->second);
        pending_tool_info_.erase(it);
    }

    if (display.empty() && !success) return;
    if (display.empty() && !pending.has_value()) return;

    int target_id = message_id_;
    auto it = tool_call_msg_ids_.find(call_id_str);
    if (it != tool_call_msg_ids_.end()) {
        target_id = it->second;
        tool_call_msg_ids_.erase(it);
    }

    // Pair the tool message's history_id (parked by on_history_message_added
    // which fires immediately before this) with its dom bubble id. Used for
    // two paths: (a) pair-delete from the parent assistant cascades to this
    // tool row via history_to_dom_; (b) the hover-reveal X on the tool bubble
    // itself routes a delete-message request whose target is the tool's
    // history_id -- LLMContext::delete_message walks back to the parent
    // assistant and pair-deletes the whole turn.
    if (pending_tool_history_id_ > 0) {
        history_to_dom_[pending_tool_history_id_] = target_id;
        run_script(wxString::Format("addDeleteButton(%d, %d);",
                                    target_id, pending_tool_history_id_));
        pending_tool_history_id_ = 0;
    }

    // S5.C -- inline diff for successful edit_file / write_file / delete_file.
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
            if (pre_mutation_fetcher_ && !path.empty())
                old_content = pre_mutation_fetcher_(path);
            diff_html = render_edit_file_diff_html(args, old_content, opts);
        } else if (tool_name == "write_file") {
            const std::string path        = args.value("path", std::string{});
            std::string       new_content = args.value("content", std::string{});
            std::optional<std::string> old_content;
            if (pre_mutation_fetcher_ && !path.empty())
                old_content = pre_mutation_fetcher_(path);
            diff_html = render_write_file_diff_html(path, old_content,
                                                    new_content, opts);
        } else if (tool_name == "delete_file") {
            const std::string path = args.value("path", std::string{});
            int line_count = 0;
            if (pre_mutation_fetcher_ && !path.empty()) {
                auto old_content = pre_mutation_fetcher_(path);
                if (old_content.has_value()) {
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
                target_id,
                "'" + chat_js_escape(wxString::FromUTF8(diff_html)) + "'"));
            if (tool_name == "edit_file"  ||
                tool_name == "write_file" ||
                tool_name == "delete_file") {
                return;
            }
        }
    }

    if (display.empty()) return;

    wxString truncated = display;
    if (truncated.length() > 500)
        truncated = truncated.Left(500) + "... (" +
                    wxString::Format("%zu", display.length() - 500) + " chars truncated)";

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
        "'" + chat_js_escape(truncated) + "'"));

    // S5.G -- token chip on tool bubbles too. Estimate from the display text
    // (the bulk of the tool message's content) + framing -- close enough to
    // the ConversationHistory.tool_result message's token_estimate for the
    // chat panel's purposes.
    if (show_per_message_tokens_ && !display.empty()) {
        int est = TokenCounter::estimate(display.ToStdString(wxConvUTF8)) + 4;
        run_script(wxString::Format("setMsgTokenChip(%d, %d);", target_id, est));
    }
}

void ChatPanel::set_context_meter(int used, int limit,
                                   int prompt_tokens, int completion_tokens,
                                   int reserve_tokens)
{
    if (completion_tokens > 0) last_completion_tokens_ = completion_tokens;
    footer_chips_->set_context_meter(used, limit, prompt_tokens,
                                      completion_tokens, reserve_tokens);
}

void ChatPanel::set_show_per_message_tokens(bool show)
{
    show_per_message_tokens_ = show;
}

// ---------------------------------------------------------------------------
// S5.G -- system-prompt bubble + per-message delete + add/delete hooks
// ---------------------------------------------------------------------------

void ChatPanel::set_system_prompt_bubble(const SystemPromptAssembly& assembly)
{
    // dom_id 0 is reserved for the system-prompt bubble. It lives outside the
    // monotonic message_id_ counter so a session reset (which zeros
    // message_id_) won't collide with this slot.
    nlohmann::json breakdown;
    breakdown["total"] = assembly.total_tokens();
    nlohmann::json sections = nlohmann::json::array();
    auto push_section = [&](const char* label, int tokens) {
        if (tokens <= 0) return;
        nlohmann::json s;
        s["label"]  = label;
        s["tokens"] = tokens;
        sections.push_back(std::move(s));
    };
    push_section("Base",      assembly.base_tokens());
    push_section("Metadata",  assembly.metadata_tokens());
    push_section("LOCUS.md",  assembly.locus_md_tokens());
    push_section("Memory",    assembly.memory_tokens());
    push_section("Tools",     assembly.manifest_tokens());
    push_section("Format",    assembly.format_addendum_tokens());
    breakdown["sections"] = std::move(sections);

    run_script(wxString::Format(
        "addSystemPromptMsg(0, %s, %s);",
        "'" + chat_js_escape(wxString::FromUTF8(assembly.full_text())) + "'",
        chat_js_escape(wxString::FromUTF8(breakdown.dump()))));
}

void ChatPanel::on_history_message_added(int history_id, MessageRole role,
                                          bool deletable)
{
    if (history_id <= 0) return;
    // Tool messages: park the history_id so on_tool_result can pair it with
    // the dom_id created in on_tool_call_pending.  Pair-delete needs the
    // tool history_id -> dom_id mapping so the dom rows actually disappear
    // when the assistant pair-delete is triggered.
    if (role == MessageRole::tool) {
        pending_tool_history_id_ = history_id;
        return;
    }
    int target_dom_id = 0;
    if (role == MessageRole::user) {
        target_dom_id = last_user_dom_id_;
        last_user_dom_id_ = 0;
    } else if (role == MessageRole::assistant) {
        // The renderer holds the most recent assistant bubble (open or sealed).
        target_dom_id = renderer_ ? renderer_->current_assistant_id() : 0;
    }
    // system role isn't user-deletable; skip mapping.

    if (target_dom_id <= 0 || !deletable) return;

    history_to_dom_[history_id] = target_dom_id;
    run_script(wxString::Format("addDeleteButton(%d, %d);",
                                target_dom_id, history_id));
}

void ChatPanel::on_history_message_deleted(int history_id)
{
    if (history_id <= 0) return;
    auto it = history_to_dom_.find(history_id);
    if (it == history_to_dom_.end()) return;
    int dom_id = it->second;
    history_to_dom_.erase(it);
    run_script(wxString::Format("removeMsg(%d);", dom_id));
}

void ChatPanel::render_loaded_history(const ConversationHistory& history)
{
    // Walk forward so call_id -> tool_name lookup (used to label tool-result
    // bubbles) only needs to look backwards into already-seen messages.
    std::unordered_map<std::string, std::string> call_id_to_tool;
    for (const auto& msg : history.messages()) {
        switch (msg.role) {
        case MessageRole::system:
            // Rendered separately as the collapsed system_prompt bubble.
            continue;

        case MessageRole::user: {
            ++message_id_;
            wxString html = user_text_to_html(wxString::FromUTF8(msg.content));
            run_script(wxString::Format(
                "addMsg(%d, 'msg-user', %s);",
                message_id_, "'" + chat_js_escape(html) + "'"));
            last_user_dom_id_ = message_id_;
            if (msg.history_id > 0) {
                history_to_dom_[msg.history_id] = message_id_;
                run_script(wxString::Format("addDeleteButton(%d, %d);",
                                            message_id_, msg.history_id));
            }
            if (show_per_message_tokens_ && msg.token_estimate > 0) {
                run_script(wxString::Format(
                    "setMsgTokenChip(%d, %d);", message_id_, msg.token_estimate));
            }
            break;
        }

        case MessageRole::assistant: {
            // Remember the tool_name for each tool_call so the matching tool-
            // result bubble below can label itself.
            for (const auto& tc : msg.tool_calls) {
                if (!tc.id.empty()) call_id_to_tool[tc.id] = tc.name;
            }
            if (msg.content.empty()) {
                // tool-only assistant turn -- no visible bubble (matches the
                // live experience where the assistant bubble seals empty and
                // gets replaced by the tool bubble).
                continue;
            }
            ++message_id_;
            std::string body = markdown_to_html(msg.content);
            run_script(wxString::Format(
                "addMsg(%d, 'msg-assistant', %s);",
                message_id_,
                "'" + chat_js_escape(wxString::FromUTF8(body)) + "'"));
            // Assistants with content are deletable; the X on an assistant
            // that has tool_calls triggers a pair-delete inside LLMContext.
            if (msg.history_id > 0) {
                history_to_dom_[msg.history_id] = message_id_;
                run_script(wxString::Format("addDeleteButton(%d, %d);",
                                            message_id_, msg.history_id));
            }
            if (show_per_message_tokens_ && msg.token_estimate > 0) {
                run_script(wxString::Format(
                    "setMsgTokenChip(%d, %d);", message_id_, msg.token_estimate));
            }
            break;
        }

        case MessageRole::tool: {
            ++message_id_;
            std::string tool_name = "tool";
            if (auto it = call_id_to_tool.find(msg.tool_call_id);
                it != call_id_to_tool.end()) {
                tool_name = it->second;
            }
            // Bubble head: just the tool-name span. The result body is
            // appended below via DOM construction so its textContent goes
            // through exactly one JS-string decode -- matches the live
            // on_tool_result path. Building inline HTML here would force a
            // second chat_js_escape pass on already-escaped content, turning
            // newlines into literal backslash-n.
            wxString head = "<span class=\"tool-name\">"
                          + chat_js_escape(wxString::FromUTF8(tool_name))
                          + "</span>";
            run_script(wxString::Format(
                "addMsg(%d, 'msg-tool', %s);",
                message_id_,
                "'" + chat_js_escape(head) + "'"));
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
                "d.appendChild(det);}",
                message_id_,
                "'" + chat_js_escape(wxString::FromUTF8(msg.content)) + "'"));
            // Map the tool history_id -> dom_id and attach the hover-reveal
            // delete X. Clicking it routes through LLMContext::delete_message
            // which walks back to the parent assistant and pair-deletes the
            // whole turn (matches the live on_tool_result wiring above).
            if (msg.history_id > 0) {
                history_to_dom_[msg.history_id] = message_id_;
                run_script(wxString::Format("addDeleteButton(%d, %d);",
                                            message_id_, msg.history_id));
            }
            if (show_per_message_tokens_ && msg.token_estimate > 0) {
                run_script(wxString::Format(
                    "setMsgTokenChip(%d, %d);", message_id_, msg.token_estimate));
            }
            break;
        }
        }
    }
    // Final highlight pass so Prism colors every <pre><code> at once.
    run_script("highlightAll();");
}

void ChatPanel::set_generation_progress(int chars, int est_tokens)
{
    footer_chips_->set_generation_progress(chars, est_tokens);
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
    renderer_->flush();
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

void ChatPanel::on_input_key(wxKeyEvent& evt)
{
    // Route nav keys to whichever popup is visible first.
    if (popups_->handle_key(evt)) return;

    const int key = evt.GetKeyCode();
    if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && !evt.ShiftDown()) {
        submit_current_input();
    } else {
        evt.Skip();
    }
}

void ChatPanel::on_input_text(wxCommandEvent& evt)
{
    popups_->update();
    refresh_action_btn();
    evt.Skip();
}

bool ChatPanel::submit_current_input()
{
    wxString text = input_->GetValue().Trim().Trim(false);
    if (text.empty()) return false;

    // GUI slash dispatch.
    if (on_slash_command_ && !text.empty() && text[0] == '/') {
        wxString body = text.Mid(1);
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
            popups_->dismiss_all();
            refresh_action_btn();
            return true;
        }
    }

    input_->Clear();
    popups_->dismiss_all();
    refresh_action_btn();

    // S4.V -- extract first @<path> mention and auto-attach.
    if (on_mention_attach_ && !popups_->mention_paths().empty()) {
        auto std_text = text.ToStdString(wxConvUTF8);
        auto mentions = parse_mentions(std_text);
        std::unordered_set<std::string> known(
            popups_->mention_paths().begin(), popups_->mention_paths().end());
        for (const auto& m : mentions) {
            if (known.find(m.path) != known.end()) {
                on_mention_attach_(m.path);
                break;
            }
        }
    }

    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-user', %s);",
        message_id_, "'" + chat_js_escape(user_text_to_html(text)) + "'"));

    // S5.G -- remember this dom_id so the matching on_history_message_added
    // (which fires once the agent thread has appended the user ChatMessage)
    // can attach a delete X to the right bubble.
    last_user_dom_id_ = message_id_;

    // S5.D -- per-message token chip: client-side heuristic estimate on the
    // raw user text (effective_content may be slightly larger due to attached-
    // context and file-change prefixes; this is close enough for the UI chip).
    if (show_per_message_tokens_) {
        int est = TokenCounter::estimate(text.ToStdString(wxConvUTF8)) + 4;
        run_script(wxString::Format("setMsgTokenChip(%d, %d);", message_id_, est));
    }

    if (on_send_)
        on_send_(text.ToStdString(wxConvUTF8));
    return true;
}

// -- Slash / mention delegation --

void ChatPanel::set_slash_commands(std::vector<SlashItem> items)
{
    popups_->set_slash_commands(std::move(items));
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
        message_id_, "'" + chat_js_escape(html) + "'"));
}

void ChatPanel::set_mention_paths(std::vector<std::string> paths)
{
    popups_->set_mention_paths(std::move(paths));
}

void ChatPanel::set_on_mention_attach(std::function<void(const std::string&)> cb)
{
    on_mention_attach_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Action button
// ---------------------------------------------------------------------------

void ChatPanel::refresh_action_btn()
{
    if (!stop_btn_) return;
    const bool has_text = input_ &&
        !input_->GetValue().Trim().Trim(false).IsEmpty();
    const bool streaming = renderer_ && renderer_->is_streaming();

    if (streaming) {
        if (has_text) {
            stop_btn_->SetLabel("Queue");
            stop_btn_->SetToolTip(
                "Queue this message. The agent will pick it up after the "
                "current turn completes.");
        } else {
            stop_btn_->SetLabel("Stop");
            stop_btn_->SetToolTip("Stop the current generation");
        }
        stop_btn_->Enable();
    } else {
        stop_btn_->SetLabel("Submit");
        stop_btn_->SetToolTip("Submit the message (Enter)");
        stop_btn_->Enable(has_text);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void ChatPanel::run_script(const wxString& js)
{
    if (!webview_) return;
    if (!page_ready_ || in_run_script_) {
        pending_scripts_.push_back(js);
        return;
    }

    in_run_script_ = true;
    webview_->RunScript(js);
    while (page_ready_ && !pending_scripts_.empty()) {
        wxString next = pending_scripts_.front();
        pending_scripts_.erase(pending_scripts_.begin());
        webview_->RunScript(next);
    }
    in_run_script_ = false;
}

wxString ChatPanel::user_text_to_html(const wxString& s)
{
    wxString norm = s;
    norm.Replace("\r\n", "\n", true);
    norm.Replace("\r",   "\n", true);
    norm.Replace("\v", "\n", true);
    norm.Replace("\f", "\n", true);
    norm.Replace(wxString::FromUTF8("\xC2\x85"),     "\n", true);
    norm.Replace(wxString::FromUTF8("\xE2\x80\xA8"), "\n", true);
    norm.Replace(wxString::FromUTF8("\xE2\x80\xA9"), "\n", true);

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

void ChatPanel::on_webview_navigating(wxWebViewEvent& evt)
{
    wxString url = evt.GetURL();

    if (!page_ready_) return;
    if (url.StartsWith("javascript:")) return;

    if (link_handler_->handle_url(url, footer_chips_->current_plan_id())) {
        evt.Veto();
        return;
    }

    spdlog::trace("WebView navigation blocked: {}", url.ToStdString());
    evt.Veto();
}

} // namespace locus
