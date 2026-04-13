# Locus — Roadmap

Three levels: **Milestone → Stage → Task**

Milestones are major shippable states. Stages are coherent units of work within a milestone.
Tasks are concrete implementation items — specific enough to start coding without further design.

## Testing Protocol

**Test workspace**: `d:\Projects\AICodeAss\` (WS1 — the Locus project itself).
Locus always runs against its own folder during development. What is indexed is what is visible.

**After every stage is complete:**
1. Build
2. Run: `locus.exe d:\Projects\AICodeAss -verbose`
3. Exercise the stage's features
4. Quit → read `.locus\locus.log`
5. Fix anything unexpected before moving to the next stage

`-verbose` drops spdlog to `trace` level: SQL queries, tool args/results, LLM stream, index events.
Default (`info`) logs only errors, warnings, and stage transitions.

---

## Overview

| Milestone | Goal | Test workspaces unlocked |
|---|---|---|
| **M0 — CLI Prototype** | Prove the hard parts work before any UI | WS1 (Locus project) |
| **M1 — Desktop App** | Usable standalone tool with full ImGui UI | WS1 |
| **M2 — Full Workspace Support** | All 3 test workspaces functional | WS1, WS2 (Wikipedia), WS3 (Docs) |
| **M3 — Connected** | Remote access, VS Code shim, web frontend | All, from any device |

---

## M0 — CLI Prototype

**Goal**: A terminal program that opens a workspace, indexes it, and runs an LLM agent loop
with tool approval via `y/n` prompts. No UI. No daemon. No API server.
Proves: agent loop, LLM streaming, tool system, workspace index, LOCUS.md injection.

### S0.1 — Project Setup ✔
- [x] CMake project skeleton: `src/`, `tests/`, `tools/`, `cmake/`
- [x] `vcpkg.json` manifest with initial deps: `spdlog`, `catch2`, `nlohmann-json`, `cpr`, `sqlite3`, `efsw`, `tree-sitter`
- [x] `CMakeLists.txt`: main target + `locus_tests` Catch2 target wired up
- [x] MIT `LICENSE` file
- [x] `.gitignore` (build/, .locus/, *.db, *.onnx)
- [x] spdlog initialised: stderr sink for CLI, rotating file sink in `.locus/locus.log`

### S0.2 — Workspace Foundation ✔
- [x] `Workspace` class: open a folder, validate path, create `.locus/` if absent
- [x] `.locus/config.json` read/write (nlohmann/json): access mode, type, exclude patterns
- [x] `LOCUS.md` reader: read from workspace root, expose as `std::string`, never write
- [x] SQLite database open/create at `.locus/index.db`; WAL mode enabled
- [x] `files` table schema (path, size, modified_at, ext, is_binary, language, indexed_at)
- [x] efsw watcher: start watching workspace root, post `FileEvent` to `std::queue` with `std::mutex`
- [x] Debounce: ignore repeat events for same path within 200ms

### S0.3 — FTS5 Index ✔
- [x] `files_fts` virtual table (FTS5, porter+unicode61 tokenizer)
- [x] `symbols` table + index on `name`, `file_id`
- [x] `headings` table + index on `file_id`
- [x] Initial workspace traversal: walk all files respecting exclude patterns + max_file_size_kb
- [x] Plain text ingestion: read content, insert into FTS5
- [x] Tree-sitter integration: detect language by extension, load grammar, parse AST
- [x] Symbol extractor: walk Tree-sitter AST → extract function/class/method/struct nodes → insert into `symbols`
- [x] Heading extractor: Markdown `#` lines, HTML `<h1>`–`<h6>` → insert into `headings`
- [x] Incremental updater: on `FileEvent`, re-index only changed file (delete old rows, re-insert)
- [x] Catch2 tests: FTS5 query returns correct files, symbol lookup finds functions by name

### S0.4 — Index Query API
- [ ] `IndexQuery` class wrapping SQLite prepared statements
- [ ] `search_text(query, options)` → FTS5 BM25, returns `vector<SearchResult>{path, line, snippet, score}`
- [ ] `search_symbols(name, kind?, language?)` → symbol table lookup with prefix match
- [ ] `get_file_outline(path)` → headings + symbols for one file, no file read
- [ ] `list_directory(path, depth)` → directory tree with file metadata from `files` table
- [ ] All queries return in < 100ms on a 10k-file workspace (Catch2 perf test)

### S0.5 — LLM Client
- [ ] `ILLMClient` interface: `stream_completion(messages, tools, callback)`
- [ ] `LMStudioClient` implementation using cpr + SSE
- [ ] SSE parser: split `data:` lines, accumulate JSON delta chunks
- [ ] OpenAI tool-call protocol: detect `tool_calls` in delta, accumulate full call JSON
- [ ] Token heuristic counter: `~4 chars = 1 token` (good enough for local models)
- [ ] Config: base URL, model name, temperature, max_tokens, context_limit
- [ ] Error handling: LM Studio not running → clear error message; timeout → retry once
- [ ] Catch2 tests: SSE parser correctly assembles streaming chunks; tool-call detection

### S0.6 — Tool System
- [ ] `ITool` abstract class (name, description, params, execute, approval_policy, preview)
- [ ] `ToolRegistry`: register, find by name, `build_schema_json()` for system prompt
- [ ] `ToolCall` struct + JSON parser (from LLM output)
- [ ] `ToolResult` struct (success, content for LLM, display for user)
- [ ] Implement `ReadFileTool`: paginated read (offset + length params), approval: auto
- [ ] Implement `WriteFileTool`: write/overwrite, approval: always
- [ ] Implement `CreateFileTool`: create new file, approval: always
- [ ] Implement `DeleteFileTool`: delete with extra warning in preview, approval: always
- [ ] Implement `ListDirectoryTool`: calls `IndexQuery::list_directory`, approval: auto
- [ ] Implement `SearchTextTool`: calls `IndexQuery::search_text`, approval: auto
- [ ] Implement `SearchSymbolsTool`: calls `IndexQuery::search_symbols`, approval: auto
- [ ] Implement `GetFileOutlineTool`: calls `IndexQuery::get_file_outline`, approval: auto
- [ ] Implement `RunCommandTool`: `CreateProcess` on Windows, stream stdout/stderr, approval: always
- [ ] Catch2 tests: schema JSON valid, ReadFileTool pagination, RunCommandTool exit code capture

### S0.7 — Agent Core
- [ ] `IFrontend` interface: `on_token`, `on_tool_call_pending`, `on_tool_result`, `on_message_complete`, `on_context_meter`
- [ ] `ILocusCore` interface: `send_message`, `tool_decision`, `register_frontend`, `unregister_frontend`
- [ ] `ConversationHistory`: `vector<Message>`, JSON serialise/deserialise
- [ ] `SystemPromptBuilder`: assemble base + LOCUS.md + workspace metadata + tool schema
- [ ] LOCUS.md token budget warning (log warning if > 500 tokens)
- [ ] Agent loop: build prompt → call LLM → stream tokens to frontend → detect tool calls → pause → await `tool_decision` → execute → inject result → resume
- [ ] Context tracker: running token count; fire `on_context_meter` after each message
- [ ] Hard stop when context would overflow: fire `on_compaction_needed` (CLI: print warning + offer strategy B/C)
- [ ] Compaction strategy B (drop tool results): strip `tool_result` messages from history
- [ ] Compaction strategy C (drop oldest): remove N oldest turns

### S0.8 — CLI Frontend
- [ ] `CliFrontend : IFrontend`
  - `on_token()` → `std::cout << token` (no newline, flush)
  - `on_tool_call_pending()` → print tool name + args + preview, prompt `[y]es / [n]o / [e]dit: `
  - `on_tool_result()` → print result display text
  - `on_message_complete()` → print `\n---`
  - `on_context_meter()` → print `[ctx: 3200/8192]` in prompt
- [ ] `main.cpp`: parse args — `<workspace_path>` (required), `--endpoint`, `--model`, `-verbose`
- [ ] `-verbose` flag: set spdlog level to `trace`; logs SQL queries, tool call args/results, LLM token stream, index events
- [ ] Default log level (`info`): logs errors, warnings, stage transitions only
- [ ] Log file always written to `.locus/locus.log` (rotating, max 10MB × 3 files)
- [ ] Graceful Ctrl+C shutdown (flush log, close SQLite)

### S0.9 — M0 Validation
- [ ] End-to-end: open Locus project folder (WS1), ask "where is ITool defined?" → agent searches index, returns correct file + line
- [ ] End-to-end: ask agent to create a new file → approval prompt → file created on disk
- [ ] End-to-end: ask agent to run `cmake --build` → approval → output streamed to terminal
- [ ] All Catch2 tests pass

---

## M1 — Desktop App

**Goal**: Core runs as a system tray daemon. ImGui/DX11 frontend connects via C++ direct interface.
Full chat UI, tool approval panels, compaction dialog. VS1 (Locus project) fully usable as a daily driver.

### S1.1 — Core / Frontend Architecture
- [ ] Harden `IFrontend` / `ILocusCore` interfaces based on M0 learnings
- [ ] `FrontendRegistry`: thread-safe register/unregister, fan-out events to all registered frontends
- [ ] Promote Agent Core to run on its own `std::thread`; all `IFrontend` callbacks dispatched from agent thread
- [ ] Session serialisation: save/load full `ConversationHistory` to `.locus/sessions/<timestamp>.json`
- [ ] Session list API: enumerate sessions for a workspace

### S1.2 — System Tray Daemon
- [ ] Windows tray icon via `Shell_NotifyIcon` (Win32)
- [ ] State icons: idle (grey) / indexing (animated) / active session (blue) / error (red)
- [ ] Right-click context menu: Open UI, Open Settings, Quit
- [ ] Single instance: named mutex on startup; second launch sends `open_ui` IPC and exits
- [ ] Startup on login: write/remove `HKCU\...\Run` key (user opt-in, off by default)
- [ ] Core shutdown: wait for agent thread to finish current turn, close DB cleanly

### S1.3 — ImGui Frontend — Skeleton
- [ ] DX11 device + swap chain + render target setup
- [ ] ImGui init with `imgui_impl_dx11` + `imgui_impl_win32`
- [ ] `ImGuiFrontend : IFrontend` class; all callbacks post events to a `std::queue` read by render thread
- [ ] Window create / show / hide / close independent of Core lifetime
- [ ] Basic layout: left sidebar (file tree) + main chat area + right panel (context / settings)

### S1.4 — ImGui Frontend — Chat UI
- [ ] Scrollable message history: user messages (right-aligned), assistant messages (left-aligned)
- [ ] Streaming: token buffer drains into last assistant message on each frame
- [ ] imgui_md: render code blocks, bold, headings in assistant messages
- [ ] Input box: `InputTextMultiline`, Enter submits, Shift+Enter inserts newline
- [ ] Context meter: coloured progress bar (green → yellow → red as % fills), always visible in footer
- [ ] LOCUS.md token cost shown as `[LOCUS.md: 120 tk]` chip in footer

### S1.5 — Tool Approval UI
- [ ] Approval panel: slides in above input box when tool call is pending
- [ ] Shows: tool name badge, formatted args (syntax-highlighted JSON), `preview()` text
- [ ] Buttons: Approve (Enter), Modify (M), Reject (Esc)
- [ ] Modify flow: editable JSON args field, confirm with Enter
- [ ] Post-execution: tool result shown as collapsed `▶ tool_result` block in chat; click to expand

### S1.6 — Context Compaction UI
- [ ] `on_compaction_needed` → modal dialog overlaid on chat
- [ ] Four strategy buttons with description text
- [ ] Strategy A: trigger LLM summarisation → show proposed summary in scrollable text area → Confirm / Revise
- [ ] Strategy C: slider for N turns + live preview of which messages will be removed
- [ ] All strategies: show before/after token counts before confirming
- [ ] Manual compaction: button in context meter area

### S1.7 — Workspace & Settings UI
- [ ] Workspace open dialog: native `IFileOpenDialog` (Win32), selects folder
- [ ] File tree panel: collapsible directory tree, file type icons, click to preview in chat
- [ ] Index status: two progress bars (FTS, Vec), "Last indexed: X min ago"
- [ ] Settings panel: endpoint URL, model, temperature, context limit, compaction threshold, exclude patterns
- [ ] Workspace config saved to `.locus/config.json` on change

### S1.8 — Crow API Server (Local Only)
- [ ] Crow server starts on Core init, binds to `127.0.0.1:PORT` (default 7700)
- [ ] `WebSocketAdapter : IFrontend` registered with `FrontendRegistry`; fans WS messages to remote clients
- [ ] WebSocket: handle all message types from protocol sketch in overview.md
- [ ] HTTP REST: `GET /workspaces`, `POST /workspaces/open`, `GET /sessions`, `GET /status`
- [ ] Protocol version header on all responses; mismatch → `426 Upgrade Required`

---

## M2 — Full Workspace Support

**Goal**: All three test workspaces functional. Semantic search operational.
Wikipedia (WS2) and personal documents (WS3) work end-to-end.

### S2.1 — Semantic Search
- [ ] ONNX Runtime C++ API initialised; model loaded from `.locus/models/<name>.onnx`
- [ ] Model downloader: fetch `all-MiniLM-L6-v2.onnx` on first semantic-search enable (with progress)
- [ ] `chunks` table schema; chunking pipeline wired into indexer
- [ ] Code chunking: Tree-sitter function/class boundaries → one chunk each; split at 80 lines
- [ ] Document chunking: heading boundaries (H2/H3); sliding window fallback
- [ ] sqlite-vec extension loaded; `chunk_vectors` virtual table created
- [ ] Background embedding thread: processes `chunks` queue at low priority
- [ ] `search_semantic(query)` tool: embed query → cosine KNN → return ranked snippets
- [ ] `search_hybrid(query)` tool: FTS5 + semantic → RRF merge → return ranked snippets
- [ ] UI: Vec index progress bar (S1.7 already has placeholder)
- [ ] Per-workspace toggle: `semantic_search.enabled` in config; UI toggle in settings
- [ ] Catch2 tests: RRF merge correctness, chunking boundary detection

### S2.2 — ZIM Reader (Wikipedia / Kiwix)
- [ ] libzim vcpkg dependency added; `zim::Archive` opens a `.zim` file
- [ ] `ZimWorkspace` wraps a `.zim` as a virtual workspace; articles are virtual "files"
- [ ] Article iterator feeds indexer: title → path, HTML content → stripped plain text
- [ ] `list_directory` maps to ZIM namespace/category browsing
- [ ] `read_file` returns stripped article text (HTML tags removed)
- [ ] Index build progress for large ZIM (English Wikipedia ~21M articles takes hours; shown in UI)
- [ ] Catch2 tests: open a small test ZIM, verify article retrieval and FTS search

### S2.3 — Document Text Extraction
- [ ] pdfium vcpkg dependency; `PdfiumExtractor`: extract text per page, detect encrypted
- [ ] miniz + pugixml: `DocxExtractor` (unzip → parse `word/document.xml`)
- [ ] miniz + pugixml: `XlsxExtractor` (unzip → parse `xl/sharedStrings.xml` + sheets)
- [ ] HTML extractor: strip tags, preserve whitespace structure
- [ ] Extractor registry: map file extension → extractor; unknown → skip
- [ ] Graceful skip: unreadable/encrypted files logged to spdlog, `files` table flagged as `is_binary=1`
- [ ] Catch2 tests: PDF extraction round-trip, DOCX heading extraction

### S2.4 — Active Edit Context (F7)
- [ ] `EditContext` struct: `file_path`, `line`, `col`, `selection_text`, `selection_start`, `selection_end`
- [ ] `ILocusCore::set_edit_context(EditContext)` — stores in Core, associates with current session
- [ ] Context injected into system prompt when non-empty: `[Editing: path:line — selection]`
- [ ] User toggle: per-message checkbox "Include edit context" (default: on when context is set)
- [ ] ImGui: visible "attached context" chip above input showing file + line; click to clear
- [ ] Standalone attach: user can right-click a file in the file tree panel → "Attach to context"

---

## M3 — Connected

**Goal**: Core accessible over LAN. VS Code sends editor context. Browser frontend works.

### S3.1 — Remote Access
- [ ] Crow: optional bind to `0.0.0.0:PORT` (off by default; toggle in settings)
- [ ] Bearer token: generate random 32-byte token on first run, store in `.locus/auth.token`
- [ ] Token display: shown in settings panel with copy button; used by remote clients
- [ ] Auth middleware: skip for `127.0.0.1`; require `Authorization: Bearer <token>` for all others
- [ ] Self-signed TLS cert: generate with mbedTLS or similar on first LAN-mode enable, store in `.locus/`
- [ ] Trust-on-first-use: remote client shows cert fingerprint, user confirms once
- [ ] Tray icon tooltip: show active remote connections count

### S3.2 — VS Code Shim
- [ ] TypeScript VS Code extension project: `package.json`, `tsconfig.json`, `src/extension.ts`
- [ ] On `vscode.window.onDidChangeTextEditorSelection`: build `EditContext`, POST to Core `/edit-context`
- [ ] On `vscode.workspace.onDidChangeTextDocument`: throttle, update context
- [ ] Extension settings: `locus.coreUrl` (default `http://127.0.0.1:7700`), `locus.token`
- [ ] Status bar item: `$(locus-icon) Locus: Connected` / `Disconnected`; click to open settings
- [ ] Activation: on workspace open; deactivation: clean up on VS Code close

### S3.3 — Web / Browser Frontend
- [ ] Single HTML file + inline CSS + vanilla JS (no build step, no framework)
- [ ] WebSocket client: connect to Core, handle all message types
- [ ] Chat UI: streaming token append, tool approval buttons, context meter bar
- [ ] Served by Crow as a static file at `GET /`
- [ ] PWA: `manifest.json` (name, icon, `display: standalone`)
- [ ] `Service Worker`: cache static assets for offline load after first visit
- [ ] Mobile-responsive layout: works on phone browser connected to PC over LAN

---

## Deferred (Post-M3)

Items from requirements Nice-to-Have — not scheduled yet:

- Multiple workspaces open simultaneously (tabbed sessions)
- Export session as markdown report
- Automated coding pipeline (plan → code → test → fix loop, user-supervised)
- Online model backends (Claude, OpenAI, Gemini)
- Full VS Code extension (embedded UI panel, not just shim)
- Tauri frontend
- Native mobile app
- Plugin system for community tools
- Line-level diff viewer for AI-proposed file changes (ImGui, dtl library)
