# Locus — GUI / UX Specification

Platform-, library-, and language-agnostic description of the user-facing
surface. Any new client (desktop port, web, mobile, alternate UI library)
should behave as described here so UX stays consistent across clients.

Implementation details (widget toolkits, rendering engines, OS APIs, file
layouts) live in the architecture docs and the code, not here. This file
answers *what the user sees and does*, not *how we built it*.

---

## 1. Guiding UX Principles

These are non-negotiable. Every screen below is a consequence of these.

1. **Operating transparency.** The user can always see what the agent is
   doing: which tool is being called, with which arguments, which result
   came back, how much context it cost. Nothing happens invisibly.
2. **User as architect, AI as heavy-lifter.** The user decides; the AI
   executes. Every irreversible or shared-state action surfaces an
   approval step by default.
3. **Instant feedback.** No input lag, no phantom waits. Every keypress,
   click, and agent token appears on screen immediately. Long work streams
   progress, never freezes the surface.
4. **Token economy is visible.** The cost of a turn is not hidden. The
   user sees context usage, LOCUS.md cost, and is warned before limits
   are exceeded — never silently truncated.
5. **Control over automation.** Features default to "ask"; "auto" is
   opt-in per-tool. The user is never surprised by what the agent did.
6. **Keyboard-first where it matters.** Chat input, command palette,
   approval decisions, navigation — all reachable without the mouse.
7. **Lean native feel.** The app should feel like a desktop tool, not a
   browser tab. Small memory footprint, fast startup, no perceptible
   jank. UI must remain responsive during LLM inference.

---

## 2. Application Shell

### 2.1 Workspace binding
One running client instance is bound to exactly one workspace folder. Opening
a different folder either:
- switches the current instance (if a single-instance policy is enforced), or
- opens a new instance against that folder.

A workspace is identified to the user by its folder name + full path in the
title bar and the "recent workspaces" list.

### 2.2 Primary window regions

The main window is divided into the following regions. All except Chat are
user-dismissible and restorable from the **View** menu.

| Region        | Default position | Purpose                                       |
|---------------|------------------|-----------------------------------------------|
| **Chat**      | Center           | Conversation with the agent. Always visible.  |
| **Files**     | Left sidebar     | Workspace file tree + index status.           |
| **Activity**  | Right sidebar    | Structured log of agent actions.              |
| **Approval**  | Bottom drawer    | Appears when a tool call needs a decision.    |
| **Status bar**| Very bottom      | Two cells: status message + context meter.    |
| **Menu bar**  | Top              | All commands also reachable by keyboard.      |

Panes are resizable, closable, and their layout persists across restarts.
Closing a pane also reflects in the View menu (its checkbox unticks), and
re-ticking the View entry re-shows the pane with its last size.

The window opens at a sensible default size (roughly 1200×800) centred on the
primary display on first run, then restores its previous geometry.

The **status bar** is split into two cells:

- Left cell — short status message: `Ready` when idle, `Agent working…`
  while a turn is in progress, the selected file path when the user selects
  something in the file tree, or `Error: …` on failure.
- Right cell — current context meter: `ctx: used/limit`. Mirrors the
  footer gauge in the chat pane; gives a glanceable reading even when the
  chat footer is out of view.

### 2.3 Background process + tray
The "core" that owns workspace state, the LLM connection, and the conversation
runs as a **background process** with a **system tray icon**. The main window
is one view onto that core — closing it does not stop the agent.

- Tray icon state: **idle / indexing / active / error** (visually distinct).
  The icon tooltip names the current state (`Locus — idle`, `Locus —
  working…`, `Locus — error`).
- **Left double-click** the tray icon: restores and raises the window. If
  already shown, just brings it to the front.
- **Right-click** opens the tray menu. The "Show/Hide Window" entry label
  flips based on current window visibility so the action is always
  self-describing. The menu also offers Quit and (planned) open-settings,
  open-recent-workspace.
- Minimizing the window hides it to tray (opt-in, user-configurable). The
  tray icon remains; the core keeps running.
- Optional: start on login.
- Single-instance: launching a second time brings the first instance forward
  (or instructs it to open the requested workspace).

---

## 3. Chat Surface

The chat is the center pane. Vertical stack, newest at the bottom, auto-scroll
to bottom while streaming (but stop auto-scrolling if the user scrolls up
themselves).

### 3.1 Message kinds

| Kind             | Visual          | Notes                                         |
|------------------|-----------------|-----------------------------------------------|
| **User**         | Right-aligned bubble, accent color. | Plain text preserved (monospaced-ish, newlines kept). |
| **Assistant**    | Left-aligned bubble, neutral color. | Markdown rendered (headings, lists, tables, code blocks, blockquotes, inline code). Code blocks syntax-highlighted. |
| **Reasoning**    | Dashed/muted block, collapsible, appears **above** the assistant bubble it belongs to. | Rendered as monospace text. Auto-collapses to "Thoughts" after the turn completes. |
| **Tool call**    | Left-aligned muted bubble. Two parts: name + short preview, then a collapsible "Result" block. | Long results are truncated with a "(N chars truncated)" marker. Full result is expandable. |
| **Error**        | Centered, error-colored. | Recoverable errors only — fatal problems use a dialog. |
| **System note**  | Styled like a tool bubble, but emitted by the client (e.g. `/help` output). | Makes clear the message is local, not from the LLM. |

### 3.2 Streaming behavior

- Assistant tokens arrive incrementally. The bubble shows a **blinking cursor**
  marker until the turn completes.
- Tokens are flushed at ~30 fps — buffered briefly to avoid thrash, not so
  long that the user feels lag.
- Markdown is re-rendered on each flush so formatting appears as it's typed.
  Code syntax highlighting runs once at turn-complete (expensive).
- Reasoning tokens stream into the reasoning block (separate from the
  assistant bubble). They may arrive before *or* interleaved with content.
- The reasoning block appears with a summary of `Thinking…` while the turn
  is live. When the turn completes, the summary flips to `Thoughts` and the
  block auto-collapses. The user can still expand it to read what the model
  was doing.
- Links in rendered messages do **not** navigate the chat view away.
  External URLs are blocked by the chat surface; clicking one either does
  nothing or opens the user's default browser. The chat never turns into an
  in-app browser.

### 3.3 Tool call display

For each tool call in a turn:
1. A tool bubble appears with `<tool_name>` on its own line + short
   human-readable preview on the next line (e.g.
   "read file src/foo.cpp, lines 1-200").
2. When the result returns, a **Result** disclosure is appended to that
   bubble. Closed by default. Opens to show the raw result (monospace,
   bounded height, scrollable).
3. Results longer than a few hundred characters are **truncated inline** in
   the bubble with a `… (N chars truncated)` marker. The full result is
   still accessible via the expanded disclosure, and is never dropped from
   the conversation history unless the user compacts it.
4. If the call was rejected or modified by the user, that is reflected on
   the bubble (e.g. tag or color change).

### 3.4 Footer (per chat pane)

A single thin bar at the bottom of the chat pane, left-to-right:

- **Context meter**: progress bar + `ctx: used/limit (N%)`.
  - Green under 60%, yellow 60–80%, red above 80%.
- **Compact** button — opens the compaction dialog (Section 6).
- **Stop** button — cancels the current turn. Only enabled while streaming.
- (spacer)
- **LOCUS.md chip** — `[LOCUS.md: N tk]` when present, hidden when absent.
  Tooltip explains it's the per-request overhead from workspace instructions.

### 3.5 Input

A multi-line text box below the chat log.

- **Enter** sends. **Shift+Enter** inserts a newline.
- Placeholder hints the affordances: `/` for commands, Enter to send,
  Shift+Enter for newline.
- While the agent is working, the input is **read-only** (not disabled —
  disabling typically forces system colors that clash with dark themes).
  User can still select and copy text. Stop button in footer ends the turn.
- On turn complete, focus returns to the input.
- Submitted user messages are added to the chat log immediately, before
  the agent responds — the user always sees their own message echoed so
  there is no doubt the submission landed.

### 3.6 Copy / selection
User can select and copy any text shown in the chat, including tool results
and reasoning blocks. Code blocks should have an affordance to copy the
whole block.

---

## 4. Slash Command System

Typing `/` at the start of the input opens a **suggestion popup** that lets
the user pick from known slash commands and tools. This is the keyboard-first
command palette for the chat.

### 4.1 Suggestion sources

Two kinds of items appear in the same list:

- **Built-in commands** — local, handled by the client (no LLM call):
  - `/help` — show this list inline in the chat.
  - `/reset` (alias `/clear`) — start a fresh conversation.
  - `/compact` — open the compaction dialog.
  - `/save` — save the current session to disk.
  - `/settings` — open the settings dialog.
  - `/quit` — (CLI) exit the client.
  - `/history`, `/sessions`, `/load <id>` — (CLI) session bookkeeping.
  Any additional GUI equivalents for menu actions should be added here.

- **Tools** — every tool registered in the workspace, tagged `[tool]`. Tool
  names come from the running core, not a hardcoded list. When the user
  picks a tool, the command is inserted into the input *as a hint*; on
  Enter it is sent as a regular user message (the LLM decides whether to
  actually call that tool). Clients MAY later add direct-invoke UI but it
  is not required for parity.

Ordering: built-in commands first, tools second. Within each group,
**prefix matches** rank above **substring matches**.

### 4.2 Trigger rules

The popup opens when the entire input text starts with `/` and no whitespace
follows the slash token yet. It closes automatically when:

- The input no longer starts with `/`.
- The user types a space/tab/newline after the command name (arguments
  phase — suggestions no longer apply).
- The user presses **Escape**.
- The user clicks outside the popup.
- The input loses focus.

### 4.3 Keyboard behavior (while popup is open)

| Key            | Action                                            |
|----------------|---------------------------------------------------|
| Up / Down      | Move the selection; wraps at both ends.           |
| Tab            | Accept the current selection.                     |
| Enter          | Accept the current selection. Does NOT send.      |
| Shift+Enter    | Insert newline in input; popup closes.            |
| Escape         | Close popup; input keeps focus and content.       |
| Any other key  | Passes through to the input; popup filter updates.|

### 4.4 Mouse behavior

- Hovering a row highlights it (live follow, not click-to-highlight).
- Clicking a row accepts it.
- Clicking outside the popup dismisses it.

### 4.5 Accepting an item

When the user accepts a suggestion, the input's content becomes:

```
/<command_name><space>
```

with the cursor at the end, ready for arguments. The popup closes. Accepting
does not submit — the user decides when to press Enter. Acceptance is
idempotent: the user can invoke the popup again immediately (e.g. to switch
to a different command).

### 4.6 Submitting a slash input

On Enter, the client inspects the input:

1. If the text starts with `/` and the first token matches a built-in
   command: dispatch locally. Do **not** send anything to the agent.
   Clear the input. Any associated output (e.g. `/help` text) appears as
   a System note in the chat.
2. Otherwise: the input is sent as a normal user message. Tool-name slash
   commands fall into this path — the LLM sees the text and may choose to
   call that tool.

### 4.7 Rendering rules for list rows

Each row is a single line:

```
/<name>  [tool]  -  <short description>
```

- `[tool]` tag is omitted for built-in commands.
- Description is truncated with ellipsis if it overflows the popup width.
- Row text uses the regular UI font; monospace not required.

### 4.8 Popup geometry

- Width tracks the input width, with a sensible minimum.
- Height fits the current filtered item count, capped at ~10 visible rows.
  There must be **no empty trailing row** when fewer items are shown.
- Positioned so it does not run off-screen; if there isn't room below the
  input it opens above.
- Does not steal focus from the input — the user keeps typing while the
  popup is open.

---

## 5. File Tree / Workspace Browser (left sidebar)

A tree view of the workspace root. Lazy-loaded — children of a folder are
fetched when it is expanded.

- The synthetic root node is hidden; top-level entries appear flush with
  the pane edge.
- Entries are sorted **directories first, then files, each group
  alphabetical**. The order is stable across rebuilds.
- Distinct icons for: open folder / closed folder / generic file / code
  file (source extensions like `.cpp`, `.py`, `.rs`, `.json`, `.cmake`, …)
  / document file (`.md`, `.txt`, `.pdf`, `.docx`, `.xlsx`, `.html`, …).
  Icons may come from the host platform's icon set; the *distinction*
  between folders, code, and docs is required, not the exact pixels.
- Clicking a file selects it (shows full path in status bar). Double-click
  or Enter on a directory toggles its expansion; on a file it fires a
  "preview/open" action (future — may route through an attached editor).
- A small **stats strip** at the top of the pane (above the tree) shows
  `Files: N | Symbols: M | Headings: K`. When semantic search has finished
  embedding the workspace, a `| Vec: V` suffix is appended.
- A thin **progress gauge** sits just under the stats strip:
  - When initial indexing or a reindex is in flight and totals are not yet
    known, the gauge **pulses** (indeterminate).
  - While embeddings are being computed and progress is known, it turns
    determinate and fills from `done/total`.
  - When idle (no indexing, embedding finished or disabled), the gauge is
    hidden.
- Respects the workspace's exclude patterns — excluded files do not appear.

---

## 6. Tool Approval Surface (bottom drawer)

Opens automatically when a tool call needs a decision. Slides in; does not
cover chat, does not pop out of the window.

### 6.1 Default mode (approve / modify / reject)

- Header: tool name (bold) on one line, the human-readable preview
  (muted) on the next.
- **Arguments editor**: the JSON arguments, rendered with syntax
  highlighting (strings / numbers / property names / keywords / operators
  picked out in distinct colors, on both light and dark palettes). The
  editor starts **read-only** — it is a display by default.
- Buttons (arranged right-aligned, with accent colors to match intent —
  approve is "positive green", reject is "destructive red", modify is
  neutral):
  - **Approve** — execute with the current args. Default-focused when the
    drawer opens so Enter confirms.
  - **Modify** — toggles the arguments editor into editable mode. The
    Approve button re-labels to **Confirm** and the Modify button to
    **Cancel Edit**. Confirm parses the JSON; on a parse error an inline
    warning is shown and the drawer stays open so the user can fix it.
    Cancel Edit reverts to the original arguments and drops back to
    read-only mode.
  - **Reject** — return a rejection to the LLM so it can try something else.
- Keyboard while the drawer is focused: **Enter** = Approve (or Confirm if
  editing), **Escape** = Reject, **M** = toggle Modify. While the user is
  actively typing inside the editable arguments or the ask-user input,
  Enter and Escape route to that field, not the drawer-level shortcuts
  (Escape still cancels; Enter in the ask-user field sends).

### 6.2 `ask_user` mode

When the agent calls the special "ask the user a question" tool, the UX
switches to a question/answer form:

- The agent's question is displayed prominently (wrapped, styled as a
  heading/question label), followed by a multi-line reply text box, then
  **Reply (Enter)** and **Cancel (Esc)** buttons.
- The reply input is auto-focused.
- Approve/Modify/Reject are replaced by Reply/Cancel; there is nothing to
  "approve" because the user *is* the tool here.
- Cancelling returns a rejection to the agent so it can proceed without an
  answer rather than hanging.
- Depending on the client, `ask_user` may surface as the bottom drawer
  *or* as a modal dialog that pops up centred on the window. Both are
  acceptable — the behavior described above is what matters.

### 6.3 Policy per tool

Each tool has an approval policy: `ask` (default), `auto` (execute without
prompting), `deny` (never execute). The user sets these globally or
per-workspace in Settings. The current policy is visible on the approval
drawer so the user understands why a call did or did not surface.

### 6.4 Non-blocking display while idle
When there is no pending call, the drawer is hidden. It does not show stale
content from the previous call.

---

## 7. Context Compaction Dialog

Reached via the footer Compact button, the `/compact` slash command, or
automatically when the context crosses the configured threshold (default
80%). Modal.

The user is presented with strategies and picks one. The app **never
silently drops content**.

| Strategy                    | Description                                                                          |
|-----------------------------|--------------------------------------------------------------------------------------|
| **A — LLM Summary**         | Summarize the conversation into a compact digest. User previews and edits first.     |
| **B — Drop tool results**   | Keep all user/assistant messages; strip verbose tool outputs. Preview shows which ones. |
| **C — Drop oldest N turns** | Slider for N. Preview of which messages will vanish.                                 |
| **D — Save and restart**    | Persist the session; start fresh with an optional briefing message (user-editable).  |

At the top of the dialog a bold **Current** line reads
`Current: used / limit tokens (pct%)`. Both the **"After compaction: ~N
tokens (pct%)"** and the **"Freed: ~N tokens"** values are shown and
update live as the user changes strategy or slides the N-turns slider,
so the saving is always visible before committing.

Strategy C's preview list shows the first ~60 characters of each message
that would be dropped, prefixed by its role (e.g. `[user] …`, `[assistant]
…`). Rows are single-line and stable as the slider moves, so the user can
see exactly which turns are going away.

Pinned items (see §9) are always excluded from summarization and deletion.
Per-workspace default strategy is configurable and pre-selected.

**Implementation status.** Strategies B (drop tool results) and C (drop
oldest N turns) are live in the current clients. Strategies A (LLM
summary) and D (save-and-restart) are planned; any client should still
expose the chooser in a way that lets them be added without a redesign.

---

## 8. Settings Dialog

Grouped sections (rendered as tabs or static-box groups depending on the
client). Changes apply on OK / Apply. Cancel reverts.

1. **LLM** — endpoint URL, model name, temperature, context length. A
   muted hint next to the context field makes clear that `0 = auto-detect
   from server`. When any LLM field changes, the dialog warns that Locus
   must be restarted for the new endpoint/model to take effect; other
   categories apply live.
2. **Indexing** — exclude glob patterns (one pattern per line, multi-line
   text box), an "Enable semantic search" checkbox, and the embedding
   model name. Changing the semantic toggle or the model reloads the
   embedder; a warning surfaces if the model file cannot be found.
3. **Tools** — per-tool approval policy chooser (`Ask` / `Auto` /
   `Deny`). Tools are listed **alphabetically** and the list is housed in
   a **scrollable region** so a long tool list never grows the dialog
   beyond the screen. Only tools whose chosen policy differs from the
   tool's built-in default are persisted — this keeps the workspace
   config file tidy.
4. **Session** *(planned)* — autosave on exit, startup behavior (open
   last workspace), start-on-login toggle.
5. **Appearance** *(planned)* — theme (system / light / dark), font size.
6. **Remote** *(planned)* — enable LAN access, port, bearer token display.

All settings are workspace-scoped unless marked "Global" next to the field.

After a successful save the status bar confirms with `Settings saved`, and
the workspace config file on disk is updated atomically.

---

## 9. Pinned Items

Users can pin specific files, regions, or snippets into context. Pinned
items:
- Are always included in the prompt, surviving all compaction strategies.
- Appear as a visible strip/list — the user can see what's pinned without
  leaving the chat.
- Can be unpinned at any time.
- Show a total token cost alongside LOCUS.md overhead.

---

## 10. Active Edit Context (editor integration)

When an external editor is attached (e.g. an IDE shim), the client receives:

- current file path,
- cursor position (line + column),
- current selection (if any).

UX rules:

- This context is **shown** to the user as an "attached" snippet before
  sending (not silently injected).
- The user can toggle inclusion per-message.
- Standalone mode without an editor: the user explicitly attaches a file
  region via a UI gesture (drag into chat, right-click → attach).

---

## 11. Activity Log (right sidebar)

Chronological structured log of agent actions. It is a **log, not a chat**:
entries are short, timestamped, typed, and the detail of any single entry
can be expanded on demand.

### 11.1 Layout

A horizontal split with the event list on top and a detail view on the
bottom. The split is user-resizable; both halves have a sensible minimum
height so neither can be collapsed to zero accidentally.

### 11.2 Event list

A virtual table (scales to many thousands of entries without slowdown)
with four columns:

| Column    | Content                                                          |
|-----------|------------------------------------------------------------------|
| Time      | Local wall-clock `HH:MM:SS` when the event was emitted.          |
| Kind      | The event classification (see below).                            |
| Summary   | One-line human-readable summary.                                 |
| Tokens    | For LLM and prompt events: total tokens, or signed delta (`+N` / `-N`) versus the previous turn. |

New entries append at the bottom and the list **auto-scrolls to the
newest** so the user can watch activity live.

### 11.3 Event kinds

The following kinds are emitted; each carries both a short `summary` and a
longer `detail` blob:

- `system_prompt` — the system prompt was assembled/seeded.
- `user_message` — the user submitted a turn.
- `llm_response` — the LLM finished streaming; token usage is included.
- `tool_call` — a tool invocation was requested.
- `tool_result` — a tool invocation finished.
- `index_event` — indexer/embedder progress or notable state changes.
- `warning` — non-fatal issue surfaced to the user.
- `error` — non-fatal error surfaced to the user.

Clients should give each kind a subtle visual hint so the user can skim
the log. Suggested treatments: red-background row for errors,
amber-background row for warnings, an accent foreground for LLM responses
and a muted foreground for index events. Exact colors are up to the
theme; the *differentiation* is what matters.

### 11.4 Detail view

Selecting a row populates the bottom pane with the full event detail:

```
[<kind>]  <summary>
tokens: total=<N> out=<N> delta=±<N>    (when applicable)
------------------------------------------------------------
<full detail text, wrapped, read-only, selectable>
```

The detail is read-only and word-wrapped so long tool results, full
prompts, or stack traces are legible without horizontal scrolling.

### 11.5 Catching up

The core keeps a buffer of recent events. Any frontend that attaches
mid-session (a web client joining, the GUI opening against a running
core, etc.) can pull the backlog at attach time and then subscribe to new
events — no activity is lost to late arrivals.

### 11.6 Lifecycle

When the conversation is reset, the activity log is cleared to match. A
future enhancement: clicking an entry scrolls the chat pane to the
related message.

---

## 12. Menus & Keyboard

Top-level menus (exact labels may vary by platform convention):

- **File** — Open Workspace…, Recent Workspaces (submenu), Settings…, Quit.
- **View** — toggle Files / Activity panes (check-items that reflect
  current pane visibility; re-checking re-shows a pane that was closed).
- **Session** — Reset Conversation, Compact Context, Save Session, Saved
  Sessions submenu (one entry per saved session, each with Open / Delete
  children), Clear All Sessions…
- **Help** — About, slash command reference, link to docs.

### 12.1 Recent Workspaces submenu

- Lists up to ~10 most-recently-opened workspaces, most recent first.
- Workspaces are deduplicated on their canonical path (case-insensitive
  on platforms where the filesystem is case-insensitive).
- An empty list shows a disabled `(none)` placeholder.
- The list is **global**, not per-workspace — it is shared across every
  workspace instance.

### 12.2 Saved Sessions submenu

- Rebuilt each time the Session menu opens, so sessions saved during the
  current run show up without restarting the app.
- Each entry label format: `<timestamp>  <first-user-message preview>  (N msgs)`.
  The preview is truncated at ~48 characters with `…`.
- Each entry is itself a submenu with **Open** and **Delete** children.
- If the disk store holds more sessions than the menu comfortably displays
  (say, more than ~50), the menu caps at that count and shows a disabled
  `(K more not shown)` tail item. Full management remains possible via
  dedicated UI in Settings or future list views.
- Delete always prompts for confirmation ("Delete session 'X'? This
  cannot be undone.").
- Clear All Sessions also prompts with a total-count confirmation.

### 12.3 Cross-surface parity

Every menu item that exists in the GUI should also be reachable as a slash
command when that makes sense (see §4.1).

### 12.4 Global shortcuts

Actually bound today:

| Shortcut         | Action                           |
|------------------|----------------------------------|
| Ctrl/Cmd + O     | Open Workspace…                  |
| Ctrl/Cmd + ,     | Open Settings                    |
| Ctrl/Cmd + Q     | Quit                             |
| Ctrl/Cmd + R     | Reset conversation               |
| Ctrl/Cmd + S     | Save session                     |
| Esc              | Close popup / dialog / reject tool|

Planned / suggested for future clients (keep them free for parity):

| Shortcut         | Action                           |
|------------------|----------------------------------|
| Ctrl/Cmd + K     | Focus chat input                 |
| Ctrl/Cmd + .     | Open compaction dialog           |

---

## 13. State, Feedback, and Errors

### Progress and state

- Indexing, embedding, and LLM inference surface progress at the site the
  user cares about: indexing in the file tree, inference in the chat (via
  the streaming cursor and footer meter), embedding at the embed gauge.
- The tray icon reflects coarse state (idle / indexing / active / error).

### Errors

- **Recoverable** (bad LLM response, network hiccup, tool failure): show
  inline in chat as an error message. The status bar left cell mirrors
  the latest error (`Error: …`). The tray icon flips to its error state
  so the user notices even when the window is hidden. Offer retry where
  applicable.
- **Configuration** (LLM unreachable, bad endpoint): show in a status
  banner at the top of the chat with a link to Settings.
- **Fatal** (cannot open workspace, DB corruption, log init failure):
  modal dialog at startup, not a surprise mid-session. The app refuses
  to launch rather than running in a half-broken state.

### Long operations

Any operation that can take more than ~200 ms must either stream progress
or be cancellable. Nothing freezes the UI thread.

---

## 14. Theming & Accessibility

- Support **light** and **dark** themes, respecting the system preference
  by default; user can force either.
- Accent color may be themeable.
- Minimum font size configurable. All text scales with the chosen size.
- Contrast in both themes must meet WCAG AA for body text.
- All interactive elements are reachable by keyboard; focus ring is visible.
- Screen-reader labels on icons and status indicators.

---

## 15. Persistence

The client persists between runs:

- Window geometry, pane layout, theme choice.
- Last-used workspace (optional auto-open).
- Recent workspaces list (global, up to ~10 entries, most-recent first,
  deduped on canonical path).
- Per-workspace: approval policies, default compaction strategy, exclude
  patterns, LLM configuration, saved sessions, chat history (if
  autosave is enabled).

Persistence is **per-workspace where user intent is workspace-scoped**
(tool policies, pins, sessions) and **global otherwise** (theme, window
geometry, recent-workspaces list).

### 15.1 Workspace switching

When the user opens a different workspace from within a running instance,
the client tears down the current workspace's subsystems (agent loop,
tool registry, LLM client, index/db) in reverse dependency order, then
constructs fresh ones for the new workspace and shows a new main window.
The user sees a clean slate for the new folder without restarting the
whole process.

### 15.2 Single-instance behaviour

Launching a second instance is surfaced to the user explicitly — either
with a message ("Locus is already running") or by forwarding the request
to the existing instance so it can switch workspace. Two instances
competing over the same `.locus/` directory is never allowed.

---

## 16. Platform Conformance

Individual clients should follow their host platform's conventions where
they conflict with this document:

- Menu placement (menu bar on macOS, in-window elsewhere).
- Shortcut modifier (Cmd on macOS, Ctrl elsewhere).
- Window chrome, native dialogs, notification style.
- Tray / dock / notification area conventions.

The *behavior* described above is binding; the *chrome* is the platform's.

---

## 17. Future-Surface Notes (for porting)

When building a new client (web, mobile, alternate desktop toolkit):

- The slash command list is data, not UI: fetch it from core (built-in
  commands + tool names), don't hardcode.
- The approval drawer is a **view** on a pending-call state, not a modal —
  multiple clients may be attached to the same core; each sees the same
  pending call.
- The chat is a **projection** of the conversation history owned by core.
  Tokens arrive by subscription. Scroll state and rendering are local.
- The LOCUS.md chip and context meter come from core state, updated push.
- Theme and window chrome are client choices; the grammar (messages, tool
  bubbles, reasoning blocks) is shared.

Anything not listed here should still follow the principles in §1 when
ambiguous: **transparent, instant, user-controlled, cheap to run.**

---

## 18. Event Vocabulary (shared by every client)

The core pushes the same set of events to every attached frontend. A new
client is "feature-complete" when it handles all of these coherently. They
are listed here so ports know what to wire up — the exact callback names
are implementation details; the *concepts* are the contract.

| Event                    | What the client should do                                           |
|--------------------------|---------------------------------------------------------------------|
| turn start               | Lock the input (or make it read-only), show a busy indicator, flip the tray state to "active". |
| assistant token          | Append to the current assistant bubble; re-render markdown at a bounded rate. |
| reasoning token          | Append to the reasoning block above the assistant bubble (creating it on first token). |
| tool-call pending        | Show the tool call in chat and surface the approval drawer (or `ask_user` variant). |
| tool result              | Hide the approval drawer; append the result to the matching tool bubble with truncation + full-result disclosure. |
| turn complete            | Finalise markdown rendering, run syntax highlighting, collapse the reasoning block to "Thoughts", re-enable input, flip the tray to "idle". |
| context meter update     | Update the chat footer gauge and the status bar ctx cell. |
| compaction needed        | Open the compaction dialog automatically. |
| session reset            | Clear the chat pane and the activity log; re-seed status bar to "Conversation reset". |
| error                    | Show the error inline in chat, mirror it in the status bar, flip the tray to "error". |
| embedding progress       | Update the file tree re-index gauge (pulse → determinate → hide). |
| activity event           | Append a row to the activity log (all of the above also fan out here for a unified timeline). |

Clients may add their own concerns on top (notifications, sound cues,
editor-attached context, remote control), but they must at minimum respond
to the events above — otherwise the user loses operating transparency.
