# 0003. Desktop GUI: Dear ImGui → wxWidgets + wxWebView

- **Status**: Accepted
- **Date**: 2026-04-14
- **Commit**: 2956663 (`Changing roadmap: wxWidgets instead of ImGui.`)

## Context

The original v1 desktop plan was **Dear ImGui + DX11**: immediate-mode UI, small binary, native
home turf for a graphics developer, markdown via `imgui_md`, syntax highlighting via
`ImGuiColorTextEdit`. Aesthetically and technically appealing.

Three practical blockers emerged as the UI scope firmed up around chat + tool approval + file
tree:

1. **Text selection and copy.** Users need to select arbitrary ranges from streamed chat
   (assistant messages, code blocks, tool output) and copy them. Dear ImGui has no real text
   selection primitive — every chat bubble would be a `InputTextMultiline` in read-only mode
   or a custom implementation. Both degrade fast on long conversations.
2. **System tray, file dialogs, menus.** Locus is a tray-resident daemon with a window. ImGui
   ships no system tray integration, no native file picker, no native menu bar. Each of these
   would be a separate Windows-API integration.
3. **Markdown + code highlighting at chat speed.** `imgui_md` renders per frame. Streaming
   partial markdown as tokens arrive (tables mid-construction, unclosed code fences) was ugly.
   We wanted the same HTML/CSS/Prism.js pipeline that works everywhere else.

Meanwhile the "no Electron" rule still had to hold — we will not bundle Chromium.

## Decision

Switch v1 to **wxWidgets + wxWebView**.

- Native widgets for the shell: menu bar, system tray, file dialogs, docking (`wxAuiManager`),
  Scintilla (`wxStyledTextCtrl`) for the tool-args JSON view.
- Chat display rendered in `wxWebView`, which on Windows is **OS-provided WebView2 (Edge)** —
  no bundled runtime, not Electron.
- Markdown via `md4c` (C library) → HTML, injected into the WebView. Streaming LLM tokens are
  flushed by a `wxTimer` via `wxWebView::RunScript()` (`DOM append`).
- Code highlighting via Prism.js (~20 KB) embedded in the WebView's HTML template.
- `IFrontend` / `ILocusCore` interface is unchanged — wxWidgets still links `locus_core`
  directly, same-process, zero overhead. Agent-thread events are marshalled to the UI thread
  via `wxQueueEvent()` + custom `wxThreadEvent` types.

## Consequences

**Wins**
- Native text selection and copy everywhere — the browser-engine subtree in the WebView gives
  us the OS-standard selection model for free.
- System tray, file dialogs, menu bar, docking are all first-class — no custom Win32.
- Chat rendering reuses a standard web stack (HTML/CSS/Prism) — known-good for markdown and
  code, easy to evolve without touching C++.
- WebView2 is OS-provided on Windows 10+. No bundled Chromium. The "no Electron" rule holds.

**Costs**
- Larger than ImGui. wxWidgets is a sizeable C++ UI framework; the binary gains a few MB.
- Retained-mode model instead of immediate-mode. More classes, more event plumbing, more
  lifetime management than ImGui. Acceptable for shell widgets; the hot surface (chat stream)
  lives inside the WebView anyway.
- Two rendering worlds to reason about: wxWidgets widgets for the shell, HTML/CSS inside
  the WebView for chat. The seam (`wxWebView::RunScript()`) is explicit and small.

## Alternatives considered

- **Stay on ImGui and hand-build text selection + tray + menus.** Weeks of Win32 glue for
  parity with what wxWidgets gives in a day. ImGui also would have needed a second markdown
  renderer for streaming edge cases.
- **Sciter.** Native HTML/CSS engine, small, no Chromium. Proprietary license for commercial
  use and a smaller community — we picked the more conservative, widely-used option.
- **Qt.** Most powerful option, heaviest dependency, LGPL dynamic-link constraints complicate
  our static-CRT distribution model.
- **WinUI 3 / native Windows.** Ties us to Windows harder than we want long-term. wxWidgets
  keeps the door open for Linux/macOS later.
- **Tauri / web UI in a browser window.** Adds a JS runtime and a separate frontend process.
  Fine later as a *remote* frontend via the Crow server — not a fit for the v1 desktop shell.
