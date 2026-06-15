#include "chat_html_template.h"

#include <spdlog/spdlog.h>

#include <wx/stdpaths.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace locus {
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

std::string build_chat_html()
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
    padding: 6px 12px;
    overflow-y: auto;
    /* `overflow-x: clip` lets the side-tether pseudo-elements extend off-
       screen without producing a horizontal scrollbar. Was implicit when the
       message bubbles fit inside the viewport; now load-bearing because the
       ::before/::after lines on every .msg point outward by 100vw. */
    overflow-x: clip;
}
/* Chat redesign: strict-shape bubbles + per-side tether line.
   Goals: maximise vertical screen utilisation (small gap + small padding),
   drop round corners (visual noise on dense logs), and let a horizontal
   1-px line from the bubble edge to the corresponding screen edge carry
   the "who sent this" cue. User -> right; assistant / tool / reasoning ->
   left. Tools and reasoning bubbles already lived on the agent side, so
   they share the assistant-side tether. */
#chat { display: flex; flex-direction: column; gap: 4px; }

.msg {
    max-width: 92%;
    padding: 6px 10px;
    border-radius: 0;
    word-wrap: break-word;
    overflow-wrap: break-word;
}
/* Side tether: a horizontal line extending from the bubble's outer edge
   to the screen edge on the bubble's "side". The line is anchored at the
   vertical midpoint of the bubble and runs 100vw outward; body sets
   `overflow-x: clip` so the off-screen extent doesn't create scrollbars. */
.msg-user::after,
.msg-assistant::before,
.msg-tool::before,
.msg-reasoning::before {
    content: '';
    position: absolute;
    top: 50%;
    height: 1px;
    width: 100vw;
    pointer-events: none;
}
.msg-user {
    align-self: flex-end;
    background: #0078d4;
    color: #fff;
    white-space: pre-wrap;
    margin-right: 14px;          /* leaves room for the tether segment */
}
.msg-user::after {
    right: -100vw;
    background: #0078d4;
}
.msg-assistant {
    align-self: flex-start;
    background: #fff;
    color: #1a1a1a;
    border: 1px solid #e0e0e0;
    margin-left: 14px;
}
.msg-assistant::before {
    left: -100vw;
    background: #b8c0c8;
}
.msg-tool {
    align-self: flex-start;
    background: #f0f4f8;
    color: #555;
    border: 1px solid #d0d8e0;
    font-size: 12px;
    max-width: 92%;
    margin-left: 14px;
}
.msg-tool::before {
    left: -100vw;
    background: #c8d0d8;
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
    border-radius: 0;
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
    border-radius: 0;
    font-size: 13px;
}

/* S4.D plan bubble. Spans full width and keeps its yellow rail on the
   left as the plan-source cue; corner rounding goes away with the rest. */
.msg-plan {
    align-self: stretch;
    background: #fff8e6;
    color: #3a2f00;
    border: 1px solid #ffd66b;
    border-left: 4px solid #ffb300;
    border-radius: 0;
    font-size: 13px;
    padding: 8px 10px;
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
    border-radius: 0;
    overflow-x: auto;
}
.msg-assistant pre code {
    display: block;
    padding: 8px;
    background: #f5f5f5;
    border: 1px solid #e0e0e0;
    border-radius: 0;
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
    border-radius: 0;
    font-size: 12px;
    max-width: 100%;
    padding: 6px 10px;
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
    border-radius: 0;
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
    border-radius: 0;
    font-size: 12px;
    max-width: 92%;
    padding: 4px 10px;
    margin-left: 14px;
}
.msg-reasoning::before {
    left: -100vw;
    /* Dashed line mirrors the bubble's dashed border so the source cue
       feels consistent across the bubble + tether. */
    background: transparent;
    border-top: 1px dashed #d0d0d0;
    height: 0;
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
    .msg-user::after { background: #4a9eff; }
    .msg-assistant {
        background: #2d2d2d; color: #d4d4d4;
        border-color: #444;
    }
    .msg-assistant::before { background: #555c66; }
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
    .msg-tool::before { background: #4a525e; }
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
    .msg-reasoning::before { border-top-color: #555; }
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
function setMsgTokenChip(id, n, rate) {
    var d = document.getElementById('msg-' + id);
    if (!d) return;
    var chip = d.querySelector('.token-chip');
    if (!chip) { chip = document.createElement('span'); chip.className = 'token-chip'; d.appendChild(chip); }
    chip.textContent = '(' + n + ' t' + (rate ? '  ' + rate : '') + ')';
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
        if (body) {
            body.textContent = text;
            // .reasoning-body has its own max-height + overflow-y:auto, so
            // scrolling the page only doesn't follow new tokens once the
            // inner body fills its 240px box. Pin the inner element to the
            // bottom too so streaming reasoning stays visible.
            body.scrollTop = body.scrollHeight;
        }
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
<script>
// Signal C++ that every function above is now defined. wxEVT_WEBVIEW_LOADED is
// not a reliable readiness gate cross-backend (it fires before this inline
// script on WKWebView/macOS), so the C++ side flushes its queued RunScript
// calls only after intercepting this navigation. Idempotent on the C++ side,
// so firing it alongside a redundant LOADED event on WebView2 is harmless.
(function() {
    function signalReady() { window.location.href = 'locus://ready'; }
    if (document.readyState === 'loading')
        document.addEventListener('DOMContentLoaded', signalReady);
    else
        signalReady();
})();
</script>
</body></html>)html";

    return html;
}
} // namespace locus
