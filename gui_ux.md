# Locus -- GUI / UX Specification

Platform-, library-, and language-agnostic description of the user-facing
surface. Any new client (alternate desktop port, web, mobile, third-party
frontend) should behave as described here so UX stays consistent.

Implementation details (widget toolkits, rendering engines, OS APIs, file
layouts) live in the architecture docs and the code, not here. This file
answers *what the user sees and does*, not *how we built it*.

Snapshot reflects shipped behaviour as of 2026-05-25 (M5 complete, M6 in
progress through S6.15). Sections explicitly tagged **In design** describe
work whose user-facing shape is fixed but the implementation has not landed
yet.

---

## 1. Guiding UX Principles

These are non-negotiable. Every screen below is a consequence of these.

1. **Operating transparency.** The user can always see what the agent is
   doing: which tool is being called, with which arguments, which result
   came back, how much context it cost, which model produced it. Nothing
   happens invisibly.
2. **User as architect, AI as heavy-lifter.** The user decides; the AI
   executes. Every irreversible or shared-state action surfaces an
   approval step by default; approval policy is configurable per tool and
   via named presets.
3. **Instant feedback.** No input lag, no phantom waits. Every keypress,
   click, and agent token appears on screen immediately. Long work streams
   progress, never freezes the surface.
4. **Token economy is visible.** The cost of a turn is not hidden. The
   user sees context usage, system-prompt section breakdowns, per-message
   token estimates, and per-turn metrics. Compaction never drops content
   silently -- it always shows what is about to go and why.
5. **Control over automation.** Features default to "ask"; "auto" is
   opt-in -- per-tool, per-preset, or per-workspace. The user is never
   surprised by what the agent did. Where automation runs (auto-compact,
   auto-commit, auto-nudge), it is annotated in the activity log.
6. **Keyboard-first where it matters.** Chat input, slash palette,
   approval decisions, tab navigation, find-in-conversation -- all
   reachable without the mouse.
7. **Lean native feel.** The app should feel like a desktop tool, not a
   browser tab. Small memory footprint, fast startup, no perceptible
   jank. UI must remain responsive during LLM inference and indexing.
8. **Sources are pluggable, not assumed.** The LLM source is one of
   several configured endpoints; switching is a one-click affordance in
   the chat panel. The user is never locked into a single backend.
9. **Parallel work is supported.** Multiple conversations live in tabs
   inside one workspace. The user can plan in one tab while a long
   build runs in another.

---

## 2. Application Shell

### 2.1 Workspace binding

One running client instance is bound to exactly one workspace folder at a
time, but may hold **multiple conversation tabs** against that workspace
(see section 3). Opening a different folder either:

- switches the current instance, tearing down all tabs after offering to
  save unsaved ones, or
- opens a new instance against that folder.

A second instance attempting to attach to the **same** workspace is
refused: the workspace is held under an exclusive on-disk lock, and the
second launch is forwarded to the first.

A workspace is identified to the user by its folder name + full path in
the title bar and the "recent workspaces" list.

### 2.2 Primary window regions

The main window is divided into the following regions. All except Chat
are user-dismissible and restorable from the **View** menu.

| Region            | Default position | Purpose |
|-------------------|------------------|---------|
| **Chat (tabbed)** | Center           | One tab per conversation. Always visible; at least one tab always exists. |
| **Files**         | Left sidebar     | Workspace file tree + index status. |
| **Activity**      | Right sidebar    | Structured log of agent actions + Metrics sub-tab. |
| **Memory Bank**   | Right side, dockable | Pinned + LRU memory entries the agent can recall (section 7). Default hidden. |
| **Terminal**      | Bottom drawer    | Live output of shell processes spawned via tools (section 9). Default hidden, auto-shows on output. |
| **Approval**      | Bottom drawer    | Appears when a tool call needs a decision (section 8). |
| **Status bar**    | Very bottom      | Multi-cell: status message + context meter + active plan progress + last auto-commit. |
| **Menu bar**      | Top              | All commands also reachable by keyboard or slash. |

Panes are resizable, closable, and their layout persists across restarts.
Closing a pane also reflects in the View menu (its checkbox unticks), and
re-ticking the View entry re-shows the pane with its last size.

The window opens at a sensible default size (roughly 1200x800) centred on
the primary display on first run, then restores its previous geometry.

The **status bar** is split into multiple cells:

- **Status message** -- short text: `Ready` when idle, `Agent working...`
  while a turn is in progress, the selected file path when the user
  selects something in the file tree, or `Error: ...` on failure.
- **Context meter** -- `ctx: used/limit`. Mirrors the per-tab chat footer
  context gauge for the active tab; gives a glanceable reading.
- **Plan progress** -- `Plan: step K/N` while a plan is running in the
  active tab (see section 4.7); empty when no plan is active.
- **Auto-commit** -- `git: <short-sha>` of the most recent per-turn
  auto-commit, if auto-commit is enabled (section 16); empty otherwise.

### 2.3 Background process + tray

The "core" that owns workspace state, the LLM connection, and conversation
history runs as a **background process** with a **system tray icon**. The
main window is one view onto that core -- closing it does not stop the
agent.

- Tray icon state: **idle / indexing / active / error** (visually
  distinct). Tooltip names the current state.
- **Left double-click** the tray icon: restores and raises the window.
  If already shown, brings it to front.
- **Right-click** opens the tray menu. "Show/Hide Window" flips label to
  match window visibility so the action is always self-describing. Also:
  Quit; recent workspaces; start-on-login toggle.
- Minimizing the window hides it to tray (opt-in, user-configurable).
- Optional: start on login (Windows; per-platform equivalents).
- Per-workspace lock prevents two instances against the same folder;
  launching a second instance against a *different* workspace opens a
  separate window.

### 2.4 Notifications

Per-event sound cues (optional, per-event, default off):

- Tool approval pending
- `ask_user` question
- Turn complete
- Compaction triggered

Each event is independently toggleable. A "Only when the window isn't
focused" gate suppresses sounds when the user is already looking. Sounds
use the host platform's standard alert palette where possible.

---

## 3. Tabs and Sessions

A workspace holds zero or more **saved sessions** on disk and one or more
**open tabs** in the running window. Tabs and sessions are the same
concept at different lifecycle stages: a tab is an active conversation; a
session is its persisted form.

### 3.1 Tab strip

A tab strip sits across the top of the chat region.

- Each tab shows its title (the session's name or, until renamed, the
  first ~48 characters of the first user message), and a close button.
- Active tab is visually distinct.
- New tab: **Ctrl/Cmd+T** or the **+** button at the end of the strip.
- Close tab: **Ctrl/Cmd+W**, the per-tab close button, or right-click
  context menu. If the tab is the last one, closing it opens a fresh one
  -- a workspace never has zero tabs.
- Rename: right-click on a tab -> Rename, or **Session > Rename Active
  Tab**. The new name persists in the session file.
- Reorder: drag-and-drop (optional; clients that can't may omit).
- Right-click context menu per tab: Rename / Close / Delete Session
  (asks for confirmation -- removes the on-disk session file along with
  its compaction archives).

### 3.2 Per-tab state

Each tab owns its own:

- Conversation history
- Agent mode (Chat / Plan / Execute -- section 4.7)
- Active plan, if any
- Attached context, if any
- Pending tool call, if any
- Terminal sub-tabs for any background processes it spawned
- LLM endpoint binding (currently inherited from the active workspace
  endpoint; per-tab override is in design with section 13)

Switching tabs swaps the entire chat surface to that tab's state. The
status bar (context / plan / commit cells) reflects the active tab. The
approval drawer is tab-specific -- a pending call surfaces only when its
owning tab is active, and the tab strip flags any tab with a pending
call.

### 3.3 Session persistence

- A session is created lazily -- no file is written until the first user
  message lands. New empty tabs cost nothing on disk.
- Every history mutation auto-saves the session JSON.
- Each session lives in its own directory under `.locus/sessions/<id>/`
  alongside its compaction archive chain (section 11).
- The **Session > Saved Sessions** submenu lists sessions that are not
  currently open as tabs, most-recently-used first. Each entry is itself
  a submenu (Open / Delete).
- **Session > Manage Sessions** opens a list view with search, multi-
  select delete, and rename.
- **Session > Clean Up Old Sessions** filters by configurable retention
  (keep last N, or delete-after-D-days) and pre-selects candidates in the
  Manage dialog.
- On workspace open, previously-open tabs (recorded in a per-workspace UI
  state file) are restored. Auto-cleanup runs first if enabled and surfaces
  any pre-selected candidates before the workspace is shown.

### 3.4 Session restore fidelity

When a saved session is reopened, the chat surface reconstructs:

- All user / assistant / tool / reasoning / plan / system-note messages
  with the same order and rendering they had when saved.
- Collapsible reasoning blocks, tool call previews + truncated results.
- Plan bubble + step status (pending / in-progress / done / failed).
- Agent mode setting.
- Attached context, if any.
- An optional activity sidecar JSONL replay (opt-in via Settings) so the
  Activity log shows the original event stream for the restored turns.

For sessions saved more than ~7 days ago, timestamps render as absolute
calendar dates instead of relative ("3 days ago" -> "May 18").

---

## 4. Chat Surface

The chat is the center pane of the active tab. Vertical stack, newest at
the bottom, auto-scroll to bottom while streaming (but stop auto-scrolling
if the user scrolls up themselves).

### 4.1 Message kinds

| Kind                  | Visual | Notes |
|-----------------------|--------|-------|
| **User**              | Right-aligned bubble, accent color. | Plain text preserved (monospaced-ish, newlines kept). Hover reveals a delete X (section 4.11). |
| **Assistant**         | Left-aligned bubble, neutral color. | Markdown rendered (headings, lists, tables, code blocks, blockquotes, inline code). Code blocks syntax-highlighted. Final assistant message (no following tool call) also hover-reveals delete X. |
| **Reasoning**         | Dashed/muted block, collapsible, appears **above** the assistant bubble it belongs to. | Monospace. While the turn is live: summary `Thinking...`. On turn complete: collapses to `Thoughts` (expandable). |
| **Tool call**         | Left-aligned muted bubble. Two parts: name + short preview, then a collapsible **Result** disclosure. | Long results truncated inline with `... (N chars truncated)` marker; full result still accessible via the disclosure and never dropped unless the user compacts it. The bubble also shows a token chip for the result. |
| **Plan**              | Centered card with numbered steps. | Each step has a status glyph (pending / in-progress / done / failed) and the step description. Plan-mode only; see section 4.7. |
| **Attached context**  | Centered slim card above the next user message. | Names the attached file/region with a close-X to detach. See section 4.9. |
| **System prompt**     | Single collapsed card at the very top of the chat. | Shows the assembled system prompt with per-section token chips (Base / Metadata / LOCUS.md / Memory / Tools / Format). Read-only. See section 4.10. |
| **File-change note**  | One-line muted prefix on the next user message bubble. | `Files changed since last turn: src/foo.cpp (modified), src/bar.cpp (added)`. Surfaces external edits (not the agent's own writes -- those are suppressed). |
| **Quality correction**| Muted note below the assistant bubble that triggered it. | `[Auto-correction: <reason>]`. Shows when the corrective-nudge layer fires (empty response, repeated tool call). |
| **Error**             | Centered, error-colored. | Recoverable errors only -- fatal problems use a dialog. |
| **System note**       | Styled like a tool bubble, emitted by the client (e.g. `/help` output). | Makes clear the message is local, not from the LLM. |

### 4.2 Streaming behavior

- Assistant tokens arrive incrementally. The bubble shows a **blinking
  cursor** marker until the turn completes.
- Tokens flush at ~30 fps -- buffered briefly to avoid thrash, not so
  long that the user feels lag.
- Markdown re-renders on each flush so formatting appears as it's typed.
  Syntax highlighting on code blocks runs once at turn-complete.
- Reasoning tokens stream into the reasoning block (separate from the
  assistant bubble). They may arrive before *or* interleaved with content.
- Per-round progress (`round K/M`) shows in the footer while a multi-
  round turn (LLM -> tool -> LLM -> ...) is in flight.
- The **reasoning watchdog** (section 14) caps a single round's reasoning
  budget. When it trips, the **Commit Now** button surfaces in the
  footer; in auto-nudge mode the action fires automatically.
- Links in rendered messages do **not** navigate the chat view away.
  External URLs either do nothing or open in the system default browser.
  The chat never turns into an in-app browser.
- Locus-internal URLs (`locus://...`) are intercepted by the client to
  trigger actions: per-message delete confirmation, plan-step decisions,
  copy-code-block, etc.

### 4.3 Tool call display

For each tool call in a turn:

1. A tool bubble appears with `<tool_name>` on its own line + a short
   human-readable preview on the next (e.g. `read file src/foo.cpp,
   lines 1-200`). MCP tools (section 14) prefix the name with the server,
   e.g. `mcp:github:create_issue`.
2. When the result returns, a **Result** disclosure is appended. Closed
   by default. Opens to show the raw result (monospace, bounded height,
   scrollable).
3. Results longer than a few hundred characters are **truncated inline**
   with a `... (N chars truncated)` marker. The full result is still
   accessible via the expanded disclosure.
4. A token chip shows the result's token cost (`~N tk`).
5. If the call was rejected, modified, or auto-corrected, the bubble
   reflects that (tag, color change, or a sibling system note).
6. If a write was refused by the truncation detector (`// rest of the
   code` placeholder text), the bubble carries the blocked phrase and
   the override hint.

### 4.4 Footer (per chat pane)

A single thin bar at the bottom of the chat pane. Composition, left to
right:

- **Context meter** -- progress bar + `ctx: used/limit (N%)`. Green
  under 60%, yellow 60-80%, red above 80%. Tooltip shows the delta vs.
  the previous turn.
- **Round progress chip** -- `round K/M` while a multi-round turn is
  alive; hidden otherwise.
- **Compacted chip** -- `compacted: N` when the session has been
  compacted; clicking opens the per-session archive directory.
- **Permission preset chip** -- a dropdown of named presets (read-only /
  ask-before-edits / allow-edits / allow-all / custom). Visual cue if a
  per-tool runtime override has put the workspace into "custom".
- **Endpoint chip** *(in design, section 13)* -- the active LLM endpoint
  name; click to switch.
- (stretch spacer)
- **Auto-compact** checkbox -- enables / disables automatic compaction
  at the soft threshold.
- **Compact** button -- opens the compaction dialog (section 11).
- **Undo** button -- reverts file-mutating tool calls from the most
  recent turn (section 4.8). Disabled when no checkpoint exists.
- **Find** button -- toggles the find-in-chat bar (section 4.12).
- **Commit Now** button -- visible only when the reasoning watchdog has
  tripped during the current round (section 14). Click cancels the
  in-flight LLM stream and injects a steering message.
- **Stop** button -- cancels the current turn. Enabled only while a
  turn is in flight.

Above the footer, two more rows are shown:

- **Mode tri-state** (section 4.7) -- three exclusive toggles: Chat /
  Plan / Execute.
- **Attached context bar** -- visible when context is attached
  (section 4.9).

### 4.5 Input

A multi-line text box below the chat log.

- **Enter** sends. **Shift+Enter** inserts a newline.
- Placeholder hints the affordances: `/` for commands, `@` for file
  references, Enter to send, Shift+Enter for newline.
- While the agent is working, the input is **read-only** (selectable but
  not editable -- disabling typically forces system colors that clash
  with dark themes). Stop button in footer ends the turn.
- On turn complete, focus returns to the input.
- Submitted user messages are added to the chat log immediately, before
  the agent responds, so there is no doubt the submission landed.

### 4.6 @-mentions

Typing `@<prefix>` while the cursor sits at start-of-input or after
whitespace / `()[],;:` opens a **file mention popup** above the input.

- Suggestions come from the workspace file index: workspace-relative
  paths, ranked basename-prefix > path-prefix > basename-substring >
  path-substring.
- Up to ~50 visible rows, capped, no empty trailing rows.
- Keyboard parity with the slash popup (section 5): Up/Down to move,
  Tab/Enter to accept, Escape to dismiss.
- Accepting inserts the path into the input *and* attaches the file as
  context for the next message (section 4.9). The user may type more
  before sending.
- Multiple mentions in one message: only the first is attached as
  context for the next turn; subsequent mentions stay as inline text
  (the agent reads them as part of the message body).

### 4.7 Agent modes (Chat / Plan / Execute)

A tri-state toggle above the input controls how the next user message is
interpreted.

- **Chat** -- normal back-and-forth. The agent may call tools freely,
  subject to approval policy. Default.
- **Plan** -- the agent is constrained to produce a **plan** (a numbered
  list of steps with optional tool hints) via the `propose_plan` tool,
  not execute it. The proposed plan appears as a centered **plan bubble**
  in the chat with **Approve** / **Reject** links. Approving flips the
  mode to Execute. Rejecting drops the plan and stays in Plan mode.
- **Execute** -- the agent walks the approved plan step by step. Each
  step's status (pending / in-progress / done / failed) updates live in
  the plan bubble and in the status bar's plan cell. Switching out of
  Execute drops the active plan.

Mode is per-tab and persisted with the session.

### 4.8 Per-turn undo

Each turn that mutates files (write / edit / delete tools) writes a
checkpoint snapshot first. The **Undo** button reverts the most recent
turn's mutations -- the chat log records a system note documenting which
files were restored. Slash equivalent: `/undo`.

Checkpoints are bounded (last N turns, configurable). Older turns are
not undoable; the **Undo** button is disabled in that case.

### 4.9 Attached context

The user can attach one file/region to the next message. Sources:

- `@<path>` mention (section 4.6).
- Drag a file from the file tree into the chat input.
- Right-click a file -> **Attach to chat**.
- Editor integration: an attached editor sends `{file, cursor,
  selection}` (section 18) which surfaces here.

The attached context renders as a slim bar between the input and the
last message: `Attached: src/foo.cpp:42-90 [x]`. Clicking **[x]**
detaches. Send: the file/region is prepended to the user message body
as a fenced code block; the bar clears.

### 4.10 System prompt bubble

A single collapsed card at the very top of the chat shows the full
assembled system prompt for transparency.

- Per-section token chips: **Base** / **Metadata** / **LOCUS.md** /
  **Memory** / **Tools** / **Format**. Clicking a chip scrolls the
  expanded view to that section.
- The bubble is read-only -- to change content, edit `LOCUS.md`, the
  memory bank, the system-prompt profile (section 12.1), or the tools
  (section 12.3).
- Hash-stable across a session for a given endpoint (prefix-cache
  invariant -- see architecture docs).

### 4.11 Per-message delete

User messages and final assistant messages (without a following tool
call) reveal a small **X** on hover. Clicking prompts confirmation; on
confirm the message is removed from history and re-broadcast to every
attached frontend. The leading system message is not deletable.

Reasoning bubbles and tool-call bubbles are NOT individually deletable
-- they're tied to the assistant message they belong to. Use compaction
(section 11) to drop them.

### 4.12 Find in chat

A find bar appears at the top of the chat pane (toggle via Ctrl/Cmd+F,
the View > Find in Conversation menu, or the footer **Find** button).

- Live search-as-you-type with `N of M` counter.
- **Enter** / **Shift+Enter** jumps to next/previous match.
- Case-sensitivity toggle.
- **Escape** closes the bar; chat keeps focus.

The bar pushes chat content down rather than overlaying it (avoids
focus-model fights with WebView-backed chat surfaces).

### 4.13 Copy / selection

User can select and copy any text shown in the chat, including tool
results, reasoning blocks, and the system prompt bubble. Code blocks
have a per-block copy affordance.

---

## 5. Slash Command System

Typing `/` at the start of the input opens a **suggestion popup** that
lets the user pick from known slash commands, tools, and prompt
templates. This is the keyboard-first command palette for the chat.

### 5.1 Suggestion sources

Three kinds of items appear in the same list, each labeled:

- **Built-in commands** -- local, handled by the client (no LLM call).
  Current set:
  - `/help` -- show this list inline in the chat.
  - `/reset` (alias `/clear`) -- start a fresh tab; the current one is
    not closed unless the user closes it explicitly.
  - `/compact [instructions]` -- open the compaction dialog (or apply
    instructions directly).
  - `/save` -- force-save the active session.
  - `/undo` -- revert the most recent file-mutating turn.
  - `/settings` -- open Settings.
  - `/forget [--hard]` -- delete the last assistant turn from history.
  - `/metrics` -- show the metrics summary inline.
  - `/export_metrics` -- write metrics to a JSON/CSV file.
  - `/reload` -- rescan the prompt-templates dirs (section 15).
  - `/memorize <text>` / `/search_memory <query>` -- memory-bank
    shortcuts (section 7).
- **Tools** -- every tool registered in the workspace, tagged `[tool]`.
  Tool names come from the running core. When the user picks a tool,
  the command is inserted into the input *as a hint*; on Enter it is
  sent as a regular user message (the LLM decides whether to actually
  call that tool). MCP tools appear with the `mcp:<server>:` prefix.
- **Prompt templates** -- user-defined templates from
  `<workspace>/.locus/prompts/` (project) and the global
  `~/.locus/prompts/` (user). Tagged `[template (project)]` /
  `[template (global)]` in the popup. Accepting a template expands its
  body (with positional / named argument substitution) into the input,
  ready to send. See section 15.

Ordering: built-in commands first, tools second, templates third.
Within each group, prefix matches rank above substring matches.

### 5.2 Trigger rules

The popup opens when the entire input text starts with `/` and no
whitespace follows the slash token yet. It closes automatically when:

- The input no longer starts with `/`.
- The user types a space/tab/newline after the command name (arguments
  phase -- suggestions no longer apply).
- The user presses **Escape**.
- The user clicks outside the popup.
- The input loses focus.

### 5.3 Keyboard behavior (while popup is open)

| Key            | Action |
|----------------|--------|
| Up / Down      | Move the selection; wraps at both ends. |
| Tab            | Accept the current selection. |
| Enter          | Accept the current selection. Does NOT send. |
| Shift+Enter    | Insert newline in input; popup closes. |
| Escape         | Close popup; input keeps focus and content. |
| Any other key  | Passes through to the input; popup filter updates. |

### 5.4 Mouse behavior

- Hovering a row highlights it (live follow, not click-to-highlight).
- Clicking a row accepts it.
- Clicking outside the popup dismisses it.

### 5.5 Accepting an item

When the user accepts a suggestion, the input becomes:

```
/<command_name><space>
```

with the cursor at the end, ready for arguments. The popup closes.
Acceptance does not submit -- the user decides when to press Enter.
Acceptance is idempotent.

For prompt templates the input becomes the **fully-expanded template
body** (after argument substitution), not the `/template-name` literal.
This makes templates feel like text expansion rather than a command.

### 5.6 Submitting a slash input

On Enter, the client inspects the input:

1. Starts with `/` + a built-in command name -> dispatch locally. Do
   **not** send anything to the agent. Output appears as a System note.
2. Starts with `/` + a prompt-template name -> the expanded body has
   already been pasted on accept; this case only fires when the user
   typed the literal `/name` without using the popup. Expand and submit
   as a user message.
3. Otherwise -> send as a normal user message. Tool-name slash commands
   fall here (the LLM sees the text and may call that tool).

### 5.7 Rendering rules for list rows

Each row is a single line:

```
/<name>  [kind]  -  <short description>
```

- `[kind]` is one of: `[tool]`, `[template (project)]`, `[template (global)]`.
  Built-in commands omit the tag.
- Description is truncated with ellipsis if it overflows.

### 5.8 Popup geometry

- Width tracks the input width, with a sensible minimum.
- Height fits the current filtered item count, capped at ~10 visible
  rows. No empty trailing rows.
- Positioned so it does not run off-screen.
- Does not steal focus from the input.

---

## 6. File Tree (left sidebar)

A tree view of the workspace root. Lazy-loaded.

- Synthetic root node is hidden; top-level entries appear flush with
  the pane edge.
- Entries sorted **directories first, then files, each group
  alphabetical**. Stable across rebuilds.
- Distinct icons for: open folder / closed folder / generic file / code
  file (source extensions) / document file (`.md`, `.txt`, `.pdf`,
  `.docx`, `.xlsx`, `.html`, ...). Icons may come from the host
  platform; only the *distinction* between folders / code / docs is
  required.
- Clicking a file selects it (shows full path in status bar). Double-
  click or Enter on a directory toggles expansion; on a file fires a
  preview/open action (may route through an attached editor).
- Right-click on a file:
  - **Attach to chat** -- inserts the file as attached context (section 4.9).
  - **Copy path** -- copies the workspace-relative path.
  - **Reveal in OS file manager** -- platform-native open.
- A **stats strip** at the top: `Files: N | Symbols: M | Headings: K`.
  When semantic search has finished embedding the workspace, a
  `| Vec: V` suffix is appended.
- A thin **progress gauge** sits just under the stats strip:
  - Initial indexing / reindex with unknown totals: **pulses**
    (indeterminate).
  - Embedding-in-progress with known totals: determinate, fills from
    `done/total`.
  - Idle: hidden.
- Respects workspace `.gitignore` plus configured exclude patterns --
  excluded files do not appear.

---

## 7. Memory Bank (right side, dockable)

The Memory Bank is a workspace-scoped, agent-readable, user-curated
collection of verbatim text entries. Replaces the older "Pinned Items"
concept.

### 7.1 What an entry is

- Free text body (one paragraph or many).
- Optional tags (free-form strings).
- Pinned flag.
- Source: `user` (typed manually) / `agent` (committed via the
  `add_memory` tool after approval) / `import` (bulk import, future).
- Created-at + last-used timestamps.
- Stable id (used for cross-linking and undo).

Entries live on disk as verbatim `.md` files plus an FTS5 index plus
optional semantic embeddings. Soft-deleted entries land in a `.deleted/`
holding pen with 30-day retention; hard-delete skips that.

### 7.2 The panel

A dockable side panel, default hidden. Toggle: **Ctrl/Cmd+M** or **View
> Memory Bank**.

- **Toolbar**: search-as-you-type box, source filter (any / user /
  agent / import), tag filter (multi-select), pinned-only toggle, "Show
  deleted" toggle, Clear button.
- **Entry list**: rows show pinned indicator, short preview, tags,
  last-used time. Multi-select for bulk ops.
- **Detail pane**: full body, tags, pinned checkbox, Save button.
  Editable in place.
- **Bulk actions**: Delete / Pin/Unpin / Add tags.
- Right-click row: Open / Pin / Delete / Hard Delete / Copy ID.

### 7.3 Agent interaction

The agent calls memory via two tools (subject to approval policy):

- `add_memory(content, tags, pinned)` -- creates a new entry. Default
  approval policy is `ask` so the user sees what's being committed.
- `search_memory(query, max_results)` -- hybrid (BM25 + cosine + recency)
  retrieval. Default approval policy is `auto`.

A search returns hits to the LLM as verbatim entries (each with its id
+ tag preview). The panel surfaces the same searches as live activity
rows.

### 7.4 System prompt slot

The "Memory" section of the system prompt (section 4.10) carries the
**pinned** entries first, then the most-recently-used entries up to a
configured token budget. The user sees the resulting slot in the
collapsed system-prompt bubble.

### 7.5 Disabling

A workspace capability flag turns memory off entirely -- the panel
auto-hides, the menu greys, and the memory tools drop from the per-turn
manifest. (Useful for sensitive workspaces where the agent should not
accumulate persistent state.)

---

## 8. Tool Approval Surface (bottom drawer)

Opens automatically when a tool call needs a decision. Slides in; does
not cover chat, does not pop out of the window. The drawer is **per
tab** -- a pending call surfaces only when its owning tab is active,
and other tabs flag the pending state on the tab strip.

### 8.1 Default mode (approve / modify / reject)

- Header: tool name (bold) on one line, the human-readable preview
  (muted) on the next.
- **Safety banner** (when applicable): if the call references paths
  outside the workspace (run_command with absolute paths, parent-
  traversal, env-var expansion), a yellow banner shows the flagged
  tokens above the buttons. Approval is forced to "ask" even if the
  preset would auto-approve.
- **Arguments editor**: JSON arguments, syntax-highlighted. Starts
  **read-only** -- display by default.
- **Buttons** (right-aligned, intent-coloured):
  - **Approve** -- execute with the current args. Default-focused so
    Enter confirms.
  - **Modify** -- toggles arguments to editable. Approve relabels to
    **Confirm**; Modify relabels to **Cancel Edit**. Confirm parses
    JSON; on parse error an inline warning shows and the drawer stays
    open. Cancel Edit reverts.
  - **Reject** -- return a rejection so the LLM tries something else.
- **Keyboard** while the drawer is focused: **Enter** = Approve (or
  Confirm), **Escape** = Reject, **M** = toggle Modify. While the user
  is typing inside an editor field, Enter and Escape route to that
  field (Escape still cancels; Enter in the ask-user field sends).
- A small footer line shows the active policy and source: e.g. `policy:
  ask (workspace override)` or `policy: auto (preset: allow-edits)`.

### 8.2 `ask_user` mode

When the agent calls the special "ask the user a question" tool, the UX
switches to a question/answer form:

- The agent's question is displayed prominently (wrapped, styled as a
  question heading), followed by a multi-line reply text box, then
  **Reply (Enter)** and **Cancel (Esc)** buttons.
- Reply input is auto-focused.
- Cancelling returns a rejection so the agent can proceed without an
  answer rather than hanging.
- Depending on the client, `ask_user` may surface as the bottom drawer
  or as a modal dialog -- both acceptable.

### 8.3 Policy

Each tool has an approval policy: `auto` (execute without prompting),
`ask` (default for most), `deny` (never execute). The user sets these
globally, per-workspace, or via **named presets** (section 8.4).

The drawer shows the resolved policy + which preset / override produced
it. If the user dismisses the drawer accidentally, the call remains
pending until acted on.

### 8.4 Permission presets

A named-preset dropdown lives in the chat footer (section 4.4) so the
user can change the whole policy table in one click.

| Preset             | Behaviour |
|--------------------|-----------|
| **read-only**      | Read tools auto; everything else asks. |
| **ask-before-edits**| Read tools auto; edit / delete / shell tools ask. Default. |
| **allow-edits**    | Read and edit tools auto; delete / shell ask. |
| **allow-all**      | Auto for every built-in. MCP tools still ask (server-trust boundary). |
| **custom**         | Sentinel -- shown when the per-tool override map doesn't match any named preset. |

`ask_user` always asks (user is the tool); MCP tools always default to
`ask` regardless of preset. A per-tool runtime override flips the preset
display to **custom** so the user knows they're off the named track.

### 8.5 Idle

When no call is pending, the drawer is hidden. It does not show stale
content from the previous call.

---

## 9. Terminal Panel (bottom drawer)

When a tool spawns a shell process (`run_command` synchronous or
`run_command_bg` background), the terminal panel surfaces its output
live. Default hidden, auto-shows on first output (configurable).

### 9.1 Layout

- A tabbed strip across the top: one sub-tab per process (synchronous
  `Run` plus one per background process).
- The active sub-tab shows a scrollback view with ANSI styling
  (foreground + background colours, bold/dim, cursor erase-in-line and
  erase-in-display).
- A single-line stdin entry sits below the scrollback for background
  processes only. Enter forwards the line to the child's stdin and
  echoes it in dim text in the scrollback. (Synchronous runs are
  modal -- nothing to type into.)

### 9.2 Per-tab behaviour

- Title shows the process command + exit status badge once finished.
- Right-click context menu: Copy / Clear / Kill (kills the child via
  the workspace's process registry; the child's Job Object terminates
  any grandchildren).
- Scrollback capped (~10 000 lines). Older lines drop in event-sized
  chunks (not byte-sized, to avoid mid-line truncation).
- A busy badge appears on the View > Terminal menu item while any
  process is live (so the user knows the panel has activity even when
  hidden).

### 9.3 Output filtering at the tool layer

Output handed back to the LLM is filtered separately from what the user
sees here -- the panel shows everything; the LLM gets a head/tail/regex/
substring-trimmed view per the tool's `output_filter_*` params. The
panel is the user-side recovery path when the agent's filtered view
isn't enough.

---

## 10. Activity Log & Metrics (right sidebar)

Chronological structured log of agent actions plus a per-session
metrics view.

### 10.1 Activity log layout

A horizontal split with the event list on top and a detail view on the
bottom. The split is user-resizable.

### 10.2 Event list

Virtual table with four columns:

| Column   | Content |
|----------|---------|
| Time     | Local wall-clock `HH:MM:SS`. |
| Kind     | Event classification. |
| Summary  | One-line human-readable summary. |
| Tokens   | LLM/prompt events: total tokens, or signed delta (`+N` / `-N`) versus the previous turn. |

New entries append at the bottom; the list auto-scrolls to the newest.

### 10.3 Event kinds (current set)

- `system_prompt` -- assembled / seeded.
- `user_message` -- user submitted a turn.
- `llm_response` -- LLM finished streaming; token usage included.
- `tool_call` / `tool_result` -- per-call lifecycle.
- `index_event` -- indexer / embedder progress.
- `memory_added` / `memory_deleted` / `memory_searched`.
- `plan_proposed` / `plan_step_advanced` / `plan_completed`.
- `auto_commit` -- per-turn git commit landed.
- `compaction` -- triggered (auto or manual); records strategy + freed
  tokens.
- `quality_correction` -- corrective nudge fired (empty response,
  repeated tool call).
- `reasoning_watchdog` -- watchdog tripped (trigger + value +
  nudge_count).
- `truncation_blocked` -- a write was refused for containing a `...
  rest of the code` placeholder.
- `endpoint_changed` -- LLM endpoint switched mid-session
  *(in design, section 13)*.
- `warning` / `error` -- non-fatal issues.

Clients should give each kind a subtle visual hint (red row for errors,
amber for warnings, accent for LLM responses, muted for index events).
Exact colours are theme choices; differentiation is what matters.

### 10.4 Detail view

Selecting a row populates the bottom pane with full detail:

```
[<kind>]  <summary>
tokens: total=<N> out=<N> delta=+/-<N>     (when applicable)
------------------------------------------------------------
<full detail text, wrapped, read-only, selectable>
```

### 10.5 Metrics sub-tab

A second tab inside the Activity panel shows per-session metrics:

- Turn-count, total tokens (prompt / completion / reasoning / cached).
- Per-tool call count + average duration + content size.
- Retrieval hit-rate (per `search_*` tool: queries -> non-empty results).
- Two spark bars: per-turn duration, per-turn total tokens.
- **Export JSON.../CSV...** buttons -- writes to a user-chosen path.

Refreshes on a 1 s timer and on every appended activity event.

### 10.6 Catching up

The core keeps a buffer of recent events. Any frontend attaching mid-
session pulls the backlog at attach time and subscribes to new events
-- no activity is lost to late arrivals.

### 10.7 Lifecycle

Activity log is per-tab. Opening a saved session optionally replays its
sidecar activity JSONL (if enabled when the session was saved). Tab
close clears that tab's log (the persisted sidecar remains on disk).

---

## 11. Context Compaction Dialog

Reached via the footer **Compact** button, the `/compact` slash command,
or automatically when the context crosses a configured threshold
(soft trigger). Modal.

The compactor is a **fixed-order cascade of independently-toggleable
layers**, not a one-of-A/B/C choice. The user picks which layers to run;
the cascade stops as soon as enough room is freed.

### 11.1 Layers

| # | Layer | What it drops |
|---|-------|---------------|
| 1 | Drop redundant tool results | Successive identical `read_file` / `search_*` results -- keep only the most recent. |
| 2 | Strip large tool result bodies | Replace bodies over a threshold with a one-line header (`<read_file: src/foo.cpp, 12 318 bytes -- body stripped>`); a companion future tool (backlog) can re-fetch by call_id. |
| 3 | Drop old reasoning | Strip `reasoning_content` from past-turn assistant messages (the current turn's reasoning is kept). |
| 4 | Drop oldest N turns | Slider for N. Preview lists the first ~60 chars of each message that would vanish, prefixed by role. |
| 5 | LLM summary | Hand the current history to the model to summarise; user previews and can edit the result before committing. |
| 6 | Save and restart | Persist the session, open a fresh tab with an optional briefing message (editable). |

Each layer is a checkbox; defaults are configurable per workspace.

### 11.2 Live preview

Top of the dialog: bold `Current: used / limit tokens (pct%)`. As the
user toggles layers and slides the N-turns slider, two values update
live:

- `After compaction: ~N tokens (pct%)`
- `Freed: ~N tokens`

Strategy 4's preview list updates in real time; row order is stable as
the slider moves so the user can see exactly which turns are going.

### 11.3 Archive chain

Every compaction writes a **history archive** before mutating:
`.locus/sessions/<id>/history.before-compact-<N>.json`. The chain is
walkable -- the user can recover any prior state via Manage Sessions.
The chat footer's **compacted chip** links to the directory.

### 11.4 Memory-bank items survive

Memory bank entries are never compacted by definition -- they live
outside the conversation. The Memory section of the system prompt is
unaffected.

### 11.5 Auto

The **Auto-compact** footer checkbox (section 4.4) enables automatic
runs at the soft threshold (default 80% of context). The first auto-run
in a session pops the dialog with default layers pre-checked so the
user sees what's about to happen; subsequent runs in the same session
just execute the same layer set silently and emit a `compaction` event
in the Activity log.

---

## 12. Settings Dialog

Grouped sections (rendered as tabs). Changes apply on OK / Apply.
Cancel reverts.

### 12.1 LLM

- Endpoint URL, model name, temperature, max tokens, context length
  (hint: `0 = auto-detect from server`).
- Tool-call wire format (auto / openai / qwen / claude / none).
- Power-user samplers (top_p / top_k / min_p / repeat_penalty /
  frequency_penalty / presence_penalty). 0 = "don't send".
- **Reset samplers to preset** button -- resets only the sampler block
  to the detected model's preset; temperature / max_tokens / tool_format
  are left alone.
- Grammar mode (off / best_effort / strict).
- System-prompt profile (Full / Compact / Minimal) -- selects the prose
  body length without touching workspace metadata / LOCUS.md / Memory /
  Tools / Format addendum.
- Auto-detect model preset (default on) -- on session open, the active
  model id is matched against the known-preset table and matching
  defaults apply.
- Reasoning watchdog (section 14): max seconds, max chars, max rounds
  silent, auto-nudge toggle.
- Strip past-turn thinking from outgoing payload (default on).
- Detect write-truncation (default on).
- Quality monitor enabled (default on).

When any LLM field changes, the dialog warns that Locus must be
restarted for the new endpoint/model to take effect -- other categories
apply live.

### 12.2 Indexing

- Exclude glob patterns (one per line).
- Max file size (KB).
- Semantic search toggle + embedding model name.
- Reranker toggle + reranker model name + reranker_top_k.
- Chunk size / overlap.

Changing the semantic toggle or model reloads the embedder. A warning
surfaces if the model file is missing.

### 12.3 Tool Approvals

- Per-tool policy chooser (`Auto` / `Ask` / `Deny`).
- Tools listed alphabetically in a scrollable region; long lists never
  grow the dialog beyond the screen.
- Only tools whose chosen policy differs from the built-in default are
  persisted (config stays tidy).
- A **preset selector** at the top (read-only / ask-before-edits /
  allow-edits / allow-all / custom). Choosing a named preset bulk-fills
  the table; manually flipping any cell switches to **custom**.

### 12.4 Capabilities

- Memory bank: enable / disable.
- Plan mode: enable / disable.
- Auto-commit: enable / disable, branch name, commit-subject prefix.
- Lazy tool manifest: enable / disable.
- File-change awareness: enable / disable.

### 12.5 Notifications

- Per-event sound toggles: tool approval, ask_user, turn complete,
  compaction.
- "Only when the window isn't focused" gate.

### 12.6 Sessions

- Auto-save: on / off.
- Auto-cleanup: on / off, keep last N, delete after D days.
- Restore tabs on workspace open: on / off.
- Activity sidecar JSONL: on / off.

### 12.7 MCP

See section 14.

### 12.8 Endpoints *(in design, section 13)*

CRUD for LLM endpoint profiles.

All settings are workspace-scoped unless marked **Global** next to the
field.

After a successful save the status bar confirms with `Settings saved`,
and the workspace config file is updated atomically.

---

## 13. LLM Endpoints *(in design)*

Multi-source LLM support. The user maintains a list of **endpoint
profiles** (LM Studio, Ollama, NVIDIA Build, OpenAI direct, OpenRouter,
Claude-via-proxy, custom) and switches the active one from the chat
footer in one click.

### 13.1 Endpoint chip

A dropdown in the chat footer (section 4.4) shows the active endpoint
name. Click opens the profile list plus an **Edit endpoints...** sentinel
that opens Settings > Endpoints.

- Tooltip shows the base URL plus the masked tail of the API key
  (`****abcd`) so the user can verify which profile is wired.
- Profiles missing a required key render with a warning colour and a
  `*` suffix.

### 13.2 Switch semantics

- Switching takes effect on the **next user message**. The current
  turn (if any) completes against the previous endpoint.
- History is preserved across the switch. The context-meter number
  may shift because tokenisers differ; this is logged but not flagged
  as an anomaly.
- Mid-turn switch is queued and applied at end-of-turn.

### 13.3 Profile fields

- Name (display label).
- Base URL.
- API key (stored at `%APPDATA%/Locus/endpoints.json` / `~/.config/
  locus/endpoints.json`; plaintext, OS file ACL is the security
  boundary; never written to logs).
- Default model.
- Tool-call format override.
- Extra headers (for providers like OpenRouter that benefit from
  `HTTP-Referer` / `X-Title` analytics headers).

### 13.4 Builtin seeds

Six profiles ship pre-seeded (all OpenAI-compatible -- the proxy form
of Claude lets one transport cover every source):

1. LM Studio (local)
2. Ollama (local)
3. NVIDIA Build (hosted)
4. OpenAI (hosted)
5. OpenRouter (hosted)
6. Claude via OpenAI-compat proxy (user-run)

Builtins can be edited but not deleted; clearing the name hides the
row.

### 13.5 Settings > Endpoints

A list-edit table with Add / Edit / Remove / Set Active. The edit
modal masks the key by default; "Show key" toggle reveals.

---

## 14. MCP Servers

The Model Context Protocol (MCP) lets Locus consume tools from external
servers (stdio subprocesses). User-facing surface:

### 14.1 Configuration

Servers are configured in two files (workspace overrides global by
name):

- Workspace: `<workspace>/.locus/mcp.json`.
- Global: platform global dir (`%APPDATA%/Locus/mcp.json` on Windows,
  XDG equivalent elsewhere).

Schema matches Claude Desktop / Cursor: `{"mcpServers":{"<name>":
{"command","args","env","enabled"}}}`. Missing files are non-errors.

### 14.2 Settings > MCP

- Status table: server name, status (not_started / initializing / ready
  / failed / crashed / stopped), error (if any), advertised tool count.
- **Restart** button per server (hot-reloads without restarting Locus).
- Server status is live -- a server that dies mid-session flips to
  `crashed` and its tools drop from the per-turn manifest.

### 14.3 Tool naming

Every advertised tool is registered under a namespaced name:
`mcp:<server>:<tool>`. The slash popup, system prompt, and approval
drawer all show the full namespaced name so the user always knows the
source.

### 14.4 Approval

MCP tools default to `ask` regardless of permission preset. Promoting
to `auto` requires an explicit per-tool override (the workspace
trust boundary lives at the per-server level; user opts into trust by
running an MCP server in the first place).

---

## 15. Prompt Templates

Reusable user-defined prompt bodies, expandable via the slash popup.

### 15.1 Storage

- Project: `<workspace>/.locus/prompts/<name>.md`.
- Global: `%APPDATA%/Locus/prompts/<name>.md` (Windows; XDG equivalent
  elsewhere).
- File format: optional YAML frontmatter (`description: ...`, `args:
  [a, b, ...]`) + body.

### 15.2 Expansion grammar

In the body:

- `{0}` / `{1}` / ... -- positional args.
- `{name}` -- named args (after `key=value` pairs in the slash call).
- `{{` / `}}` -- literal braces.
- Missing references render as literal placeholders so the user notices.

### 15.3 Live reload

The registry caches per-file mtime. `list()` / `find()` / `expand()`
pick up edits lazily; `/reload` forces a full directory rescan.

### 15.4 No execution context

Templates have no access to filesystem, shell, or tools. They are pure
text substitution. This is intentional -- the security model.

### 15.5 In the slash popup

Templates appear alongside built-ins and tools, tagged `[template
(project)]` or `[template (global)]`. Accepting expands the body into
the input verbatim (with argument substitution).

---

## 16. Reasoning Watchdog & Commit-Now

Reasoning-heavy local models can spiral inside a single LLM round and
never emit a tool call. The watchdog caps a single round's reasoning
budget.

### 16.1 Triggers (OR semantics)

Three configurable thresholds in Settings > LLM (section 12.1); each
`0 = disabled`:

- Max seconds since round start.
- Max accumulated reasoning chars in the round.
- Max consecutive LLM rounds without a tool call.

### 16.2 Action

- **Manual mode** (default): when a threshold trips, a non-modal
  **Commit Now** button surfaces in the chat footer (section 4.4).
  The user clicks; the in-flight stream cancels cleanly and an
  internal steering message (`Stop reasoning. Commit to a tool call
  now, or give a brief final answer.`) is appended. A new round starts.
- **Auto-nudge mode**: same action fires automatically when the
  threshold trips. Designed for agentic-harness driving.

### 16.3 Hard cap

Two nudges per user turn. The third would-fire aborts the turn with
`Agent appears stuck (3 reasoning watchdog trips in one turn).
Stopping.`

### 16.4 Activity log

Each trip emits a `reasoning_watchdog` activity event with the
triggered threshold, the measured value, and the nudge count.

---

## 17. Auto-Commit (per-turn git)

When the workspace is a git repo and the **Auto-commit** capability is
enabled (Settings > Capabilities, section 12.4):

- After every turn that touched files, Locus runs `git add -A && git
  commit -F <tempfile>`.
- Commit subject: `<configurable prefix>: <first ~72 chars of the
  user message>`.
- If a branch name is configured and the current branch differs, Locus
  attempts a switch (creates the branch if missing; logs a warning if
  the switch fails) and proceeds on the configured branch.
- "Nothing to commit" is logged as skipped, not an error.
- The resulting short SHA shows in the status bar's auto-commit cell
  (section 2.2) and in the Activity log.

The user can disable, change the branch, or change the subject prefix
at any time. Locus never force-pushes; remote operations are out of
scope.

---

## 18. Active Edit Context (editor integration)

When an external editor is attached (e.g. an IDE shim), the client
receives:

- current file path,
- cursor position (line + column),
- current selection (if any).

UX rules:

- The context is **shown** to the user as an attached snippet
  (section 4.9) before sending -- not silently injected.
- The user can detach per-message.
- Standalone mode (no editor attached): the user explicitly attaches
  via `@`-mention, drag-and-drop, or right-click on a file tree entry.

*(Editor integration is planned for VS Code via a thin shim that sends
this context over a local socket; the chat surface already supports
the receiving side via the attached-context bar.)*

---

## 19. Menus & Keyboard

Top-level menus (exact labels may vary by platform convention):

- **File** -- Open Workspace..., Recent Workspaces (submenu), Open
  Workspace Folder, Open Global Locus Folder, Settings..., Open Global
  Config..., Quit.
- **View** -- Files Panel (check), Activity Panel (check), Terminal
  (check), Memory Bank (check), Find in Conversation.
- **Session** -- New Tab, Close Tab, Rename Active Tab, Saved Sessions
  (submenu), Compact Context, Clean Up Old Sessions..., Manage
  Sessions...
- **Help** -- About, slash command reference, link to docs.

### 19.1 Recent Workspaces submenu

- Lists up to ~10 most-recently-opened workspaces, most recent first.
- Deduplicated on canonical path (case-insensitive on case-insensitive
  filesystems).
- Empty list: disabled `(none)` placeholder.
- **Global**, not per-workspace -- shared across instances.

### 19.2 Saved Sessions submenu

- Rebuilt each time the Session menu opens.
- Filters out sessions currently open as tabs.
- Each entry label: `<timestamp>  <first-user-message preview>  (N
  msgs)`. Preview truncated at ~48 chars with `...`.
- Each entry is a submenu with **Open** and **Delete** children.
- If the disk store holds more than ~50 sessions, the menu caps and
  shows a disabled `(K more not shown)` tail; full management lives
  in **Session > Manage Sessions**.
- Delete prompts for confirmation.

### 19.3 Cross-surface parity

Every menu item that exists in the GUI should also be reachable as a
slash command when that makes sense (section 5.1).

### 19.4 Global shortcuts (currently bound)

| Shortcut          | Action |
|-------------------|--------|
| Ctrl/Cmd + O      | Open Workspace... |
| Ctrl/Cmd + ,      | Open Settings |
| Ctrl/Cmd + Q      | Quit |
| Ctrl/Cmd + T      | New Tab |
| Ctrl/Cmd + W      | Close Tab |
| Ctrl/Cmd + F      | Find in Conversation |
| Ctrl/Cmd + M      | Toggle Memory Bank panel |
| Ctrl/Cmd + \`     | Toggle Terminal panel |
| Ctrl/Cmd + S      | Save session |
| Esc               | Close popup / dialog / reject tool |

Suggested for future / parity in other clients (keep free):

| Shortcut          | Action |
|-------------------|--------|
| Ctrl/Cmd + K      | Focus chat input |
| Ctrl/Cmd + .      | Open compaction dialog |
| Ctrl/Cmd + E      | Toggle endpoint picker (S6.16) |

---

## 20. State, Feedback, and Errors

### Progress and state

- Indexing, embedding, and LLM inference surface progress at the site
  the user cares about: indexing in the file tree, inference in chat
  (streaming cursor + footer meter + round chip), embedding at the
  file-tree gauge, terminal output in the terminal panel.
- Status bar shows the active tab's snapshot (status / context / plan
  / commit).
- Tray icon reflects coarse state (idle / indexing / active / error).

### Errors

- **Recoverable** (bad LLM response, network hiccup, tool failure):
  inline in chat as an error message. Status bar mirrors `Error: ...`.
  Tray flips to error state so the user notices even when the window
  is hidden. Retry offered where applicable.
- **Configuration** (LLM unreachable, bad endpoint): status banner at
  the top of chat with a link to Settings.
- **Fatal** (cannot open workspace, DB corruption, log init failure):
  modal dialog at startup, not a surprise mid-session. App refuses to
  launch rather than running in a half-broken state.

### Long operations

Any operation that can take more than ~200 ms must either stream
progress or be cancellable. Nothing freezes the UI thread.

---

## 21. Theming & Accessibility

- Support **light** and **dark** themes, respecting the system
  preference by default; user can force either.
- Accent color themeable.
- Minimum font size configurable; all text scales with the chosen size.
- Contrast in both themes must meet WCAG AA for body text.
- All interactive elements reachable by keyboard; focus ring visible.
- Screen-reader labels on icons and status indicators.
- All major widgets carry a stable accessible name (used by the UI
  automation harness on Windows; clients on other platforms should
  expose equivalent IDs for headless testing).
- Sound notifications (section 2.4) supplement but never replace
  visual cues.

---

## 22. Persistence

The client persists between runs.

### 22.1 Per-workspace (`.locus/`)

- Workspace config (`config.json`): LLM settings, indexing, tool
  policies, capabilities, notifications, sessions, system-prompt
  profile, watchdog thresholds.
- Sessions: one directory per session under `sessions/<id>/` carrying
  the JSON history + chained compaction archives + optional activity
  sidecar.
- Memory bank: one verbatim `.md` per entry under `memory/`, plus FTS
  + vector indexes. Soft-deleted entries land in `memory/.deleted/`.
- Per-workspace UI state: open tabs, last-active tab.
- MCP server list (workspace overlay).
- Prompt templates (workspace overlay).
- Index database (`index.db`) + optional vectors (`vectors.db`).
- Activity log file (`locus.log`).
- Per-session lock (`locus.lock`) preventing dual-instance attach.

### 22.2 Global

- Window geometry, pane layout, theme choice.
- Recent workspaces list (up to ~10, deduped on canonical path).
- Endpoint profiles (`%APPDATA%/Locus/endpoints.json` -- section 13).
- MCP server list (global overlay).
- Prompt templates (global overlay).

Persistence is **per-workspace where user intent is workspace-scoped**
(tool policies, memory, sessions, mcp overlay) and **global otherwise**
(theme, window geometry, recent workspaces, endpoints).

### 22.3 Workspace switching

Opening a different workspace from within a running instance tears
down the current workspace's subsystems in reverse dependency order,
then constructs fresh ones for the new workspace. Open tabs of the
previous workspace are auto-saved before teardown.

### 22.4 Single-instance behaviour

Launching a second instance against the **same** workspace is refused
(workspace lock). Against a **different** workspace it opens a second
window. Two instances competing over the same workspace is never
allowed.

---

## 23. Platform Conformance

Individual clients should follow their host platform's conventions
where they conflict with this document:

- Menu placement (menu bar on macOS, in-window elsewhere).
- Shortcut modifier (Cmd on macOS, Ctrl elsewhere).
- Window chrome, native dialogs, notification style.
- Tray / dock / notification area conventions.
- File-dialog presentation.

The *behavior* described above is binding; the *chrome* is the
platform's.

---

## 24. Future-Surface Notes (for porting)

When building a new client (web, mobile, alternate desktop toolkit):

- **Subscribe, don't poll.** The core pushes events (section 25); a
  new client wires those to UI updates. Polling is wrong by default --
  it loses operating transparency between ticks.
- **Tabs are core state, not client state.** A tab id is meaningful
  across attached frontends; multiple clients seeing the same workspace
  should agree on which tab is which.
- **The slash list is data, not UI.** Fetch built-ins + tool names +
  template names from core; don't hardcode.
- **Approval drawer is a view on a pending-call state, not a modal.**
  Multiple clients may be attached to the same core; each sees the
  same pending call. First decision wins.
- **The chat is a projection of conversation history owned by core.**
  Tokens arrive by subscription. Scroll state and rendering are local.
- **The system prompt bubble is push-state.** Re-renders on every
  prompt rebuild; clients display, never edit.
- **Memory bank is a separate store with its own search.** Don't try
  to fold it into the conversation history projection -- it has its
  own list / filter / detail UX (section 7).
- **Theme, window chrome, and platform shortcuts are client choices.**
  The grammar (messages, tool bubbles, reasoning, plan, attached,
  system-prompt, memory) is shared.

Anything not listed here should follow the principles in section 1
when ambiguous: **transparent, instant, user-controlled, cheap to run.**

---

## 25. Event Vocabulary (shared by every client)

The core pushes the same set of events to every attached frontend. A
new client is "feature-complete" when it handles all of these
coherently. Callback names are implementation details; the *concepts*
are the contract.

| Event                       | What the client should do |
|-----------------------------|---------------------------|
| tab opened                  | Add a tab to the strip; if active, switch the chat surface to it. |
| tab closed                  | Remove from the strip; if it was active, activate the next/previous tab; ensure at least one tab remains. |
| tab activated               | Swap chat / approval / terminal / status bar to this tab's state. |
| history message added       | Append a bubble of the matching kind; assign a stable dom-id matched to the message's history id. |
| history message deleted     | Remove the matching bubble; update token totals. |
| system prompt rebuilt       | Re-render the system-prompt bubble at the top of the chat with new section token chips. |
| turn start                  | Lock the input (read-only), show busy indicator, flip tray to active. |
| round progress              | Update the round chip in the footer (`round K/M`). |
| assistant token             | Append to the current assistant bubble; re-render markdown at bounded rate. |
| reasoning token             | Append to the reasoning block above the assistant bubble (creating it on first token). |
| reasoning watchdog tripped  | Show the **Commit Now** button in the footer. |
| reasoning watchdog cleared  | Hide the **Commit Now** button. |
| tool-call pending           | Show the tool call in chat and surface the approval drawer (or `ask_user` variant). |
| tool result                 | Hide the drawer; append result to the matching tool bubble with truncation + full-result disclosure. |
| quality correction          | Append a muted correction note below the assistant bubble. |
| truncation blocked          | Update the matching tool bubble with the blocked-phrase explanation. |
| plan proposed               | Render the plan bubble with Approve / Reject. |
| plan step advanced          | Update the step's status glyph; update the status bar plan cell. |
| plan completed              | Mark the plan bubble done / failed; clear the status bar plan cell. |
| attached context set        | Show the attached-context bar above the input. |
| attached context cleared    | Hide the attached-context bar. |
| memory event                | If the Memory Bank panel is open, re-render the list; otherwise nothing visible. |
| auto-commit landed          | Update the status bar commit cell; append an activity row. |
| terminal chunk              | If Terminal panel is open and the matching sub-tab is active, append to the scrollback; otherwise enqueue. |
| terminal process exited     | Update the sub-tab's exit badge; flip the busy indicator if no other process is alive. |
| turn complete               | Finalise markdown rendering, run syntax highlighting, collapse reasoning to "Thoughts", re-enable input, flip tray to idle. |
| context meter update        | Update the chat footer gauge and the status bar context cell. |
| compaction needed           | Open the compaction dialog (or run silently in subsequent-auto mode). |
| compaction completed        | Update the compacted chip; append activity row. |
| endpoint changed *(in design)* | Update the endpoint chip label + tooltip; refresh model/context cells. |
| embedding progress          | Update the file tree re-index gauge (pulse -> determinate -> hide). |
| index event                 | Refresh the file-tree stats strip. |
| session reset / new tab     | Open a fresh tab; reset the chat pane and the activity log for the new tab. |
| error                       | Show the error inline in chat, mirror in status bar, flip tray to error. |
| activity event              | Append a row to the activity log (every event above also fans out here for a unified timeline). |

Clients may add their own concerns on top (notifications, sound cues,
editor-attached context, remote control), but they must at minimum
respond to the events above -- otherwise the user loses operating
transparency.
