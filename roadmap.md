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
| **M1 — Desktop App** | Usable standalone tool with wxWidgets UI | WS1 |
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

### S0.4 — Index Query API ✔
- [x] `IndexQuery` class wrapping SQLite prepared statements
- [x] `search_text(query, options)` → FTS5 BM25, returns `vector<SearchResult>{path, line, snippet, score}`
- [x] `search_symbols(name, kind?, language?)` → symbol table lookup with prefix match
- [x] `get_file_outline(path)` → headings + symbols for one file, no file read
- [x] `list_directory(path, depth)` → directory tree with file metadata from `files` table
- [x] All queries return in < 100ms on a 10k-file workspace (Catch2 perf test)

### S0.5 — LLM Client ✔
- [x] `ILLMClient` interface: `stream_completion(messages, tools, callback)`
- [x] `LMStudioClient` implementation using cpr + SSE
- [x] SSE parser: split `data:` lines, accumulate JSON delta chunks
- [x] OpenAI tool-call protocol: detect `tool_calls` in delta, accumulate full call JSON
- [x] Token heuristic counter: `~4 chars = 1 token` (good enough for local models)
- [x] Config: base URL, model name, temperature, max_tokens, context_limit
- [x] Error handling: LM Studio not running → clear error message; timeout → retry once
- [x] Catch2 tests: SSE parser correctly assembles streaming chunks; tool-call detection

### S0.6 — Tool System ✔
- [x] `ITool` abstract class (name, description, params, execute, approval_policy, preview)
- [x] `ToolRegistry`: register, find by name, `build_schema_json()` for system prompt
- [x] `ToolCall` struct + JSON parser (from LLM output)
- [x] `ToolResult` struct (success, content for LLM, display for user)
- [x] Implement `ReadFileTool`: paginated read (offset + length params), approval: auto
- [x] Implement `WriteFileTool`: write/overwrite, approval: always
- [x] Implement `CreateFileTool`: create new file, approval: always
- [x] Implement `DeleteFileTool`: delete with extra warning in preview, approval: always
- [x] Implement `ListDirectoryTool`: calls `IndexQuery::list_directory`, approval: auto
- [x] Implement `SearchTextTool`: calls `IndexQuery::search_text`, approval: auto
- [x] Implement `SearchSymbolsTool`: calls `IndexQuery::search_symbols`, approval: auto
- [x] Implement `GetFileOutlineTool`: calls `IndexQuery::get_file_outline`, approval: auto
- [x] Implement `RunCommandTool`: `CreateProcess` on Windows, stream stdout/stderr, approval: always
- [x] Catch2 tests: schema JSON valid, ReadFileTool pagination, RunCommandTool exit code capture

### S0.7 — Agent Core ✔
- [x] `IFrontend` interface: `on_token`, `on_tool_call_pending`, `on_tool_result`, `on_message_complete`, `on_context_meter`
- [x] `ILocusCore` interface: `send_message`, `tool_decision`, `register_frontend`, `unregister_frontend`
- [x] `ConversationHistory`: `vector<Message>`, JSON serialise/deserialise
- [x] `SystemPromptBuilder`: assemble base + LOCUS.md + workspace metadata + tool schema
- [x] LOCUS.md token budget warning (log warning if > 500 tokens)
- [x] Agent loop: build prompt → call LLM → stream tokens to frontend → detect tool calls → pause → await `tool_decision` → execute → inject result → resume
- [x] Context tracker: running token count; fire `on_context_meter` after each message
- [x] Hard stop when context would overflow: fire `on_compaction_needed` (CLI: print warning + offer strategy B/C)
- [x] Compaction strategy B (drop tool results): strip `tool_result` messages from history
- [x] Compaction strategy C (drop oldest): remove N oldest turns

### S0.8 — CLI Frontend ✔
- [x] `CliFrontend : IFrontend`
  - `on_token()` → `std::cout << token` (no newline, flush)
  - `on_tool_call_pending()` → print tool name + args + preview, prompt `[y]es / [n]o / [e]dit: `
  - `on_tool_result()` → print result display text
  - `on_message_complete()` → print `\n---`
  - `on_context_meter()` → print `[ctx: 3200/8192]` in prompt
- [x] `main.cpp`: parse args — `<workspace_path>` (required), `--endpoint`, `--model`, `-verbose`
- [x] `-verbose` flag: set spdlog level to `trace`; logs SQL queries, tool call args/results, LLM token stream, index events
- [x] Default log level (`info`): logs errors, warnings, stage transitions only
- [x] Log file always written to `.locus/locus.log` (rotating, max 10MB × 3 files)
- [x] Graceful Ctrl+C shutdown (flush log, close SQLite)

### S0.9 — M0 Validation ✔
- [x] End-to-end: open Locus project folder (WS1), ask "where is ITool defined?" → agent searches index, returns correct file + line
- [x] End-to-end: ask agent to create a new file → approval prompt → file created on disk
- [x] End-to-end: ask agent to run `cmake --build` → approval → output streamed to terminal
- [x] All Catch2 tests pass (70/70; 1 flaky Windows file-handle race on re-run)

---

## M1 — Desktop App

**Goal**: Core runs as a system tray daemon. wxWidgets frontend connects via C++ direct interface.
Full chat UI, tool approval panels, compaction dialog. WS1 (Locus project) fully usable as a daily driver.

### S1.1 — Core / Frontend Architecture
- [ ] Harden `IFrontend` / `ILocusCore` interfaces based on M0 learnings
- [ ] `FrontendRegistry`: thread-safe register/unregister, fan-out events to all registered frontends
- [ ] Promote Agent Core to run on its own `std::thread`; all `IFrontend` callbacks dispatched from agent thread
- [ ] Session serialisation: save/load full `ConversationHistory` to `.locus/sessions/<timestamp>.json`
- [ ] Session list API: enumerate sessions for a workspace

### S1.2 — wxWidgets Bootstrap + System Tray
- [ ] Add `wxwidgets` (with `webview` feature) + `md4c` to `vcpkg.json`
- [ ] `locus_gui` CMake target (WIN32 exe), links `locus_core` + `wx::core wx::base wx::aui wx::stc wx::webview` + `md4c::md4c-html`
- [ ] `LocusApp : wxApp` — entry point, owns Workspace + AgentCore lifetime
- [ ] `LocusFrame : wxFrame` + `wxAuiManager` — dockable 3-pane layout (sidebar, chat, right panel)
- [ ] `LocusTray : wxTaskBarIcon` — state icons (idle/active/error), right-click menu, minimize-to-tray
- [ ] `wxSingleInstanceChecker` for single-instance enforcement
- [ ] `WxFrontend : IFrontend` — thread bridge: all callbacks post `wxThreadEvent` via `wxQueueEvent()` to main thread
- [ ] Startup on login: write/remove `HKCU\...\Run` key (user opt-in, off by default)
- [ ] Core shutdown: wait for agent thread to finish current turn, close DB cleanly
- [ ] Source layout: `src/gui/*.h/cpp`

### S1.3 — Chat UI + Markdown
- [ ] `wxWebView` (Edge/WebView2, OS-provided) as chat display panel — HTML/CSS rendering surface, not a bundled browser
- [ ] `md4c` converts markdown → HTML before injection into WebView
- [ ] Streaming: `wxTimer` (30–50ms) flushes token buffer via `wxWebView::RunScript()` (DOM append)
- [ ] Syntax highlighting in code blocks via embedded Prism.js (~20KB)
- [ ] Scrollable message history: user messages (right-aligned), assistant messages (left-aligned)
- [ ] `wxTextCtrl` (`wxTE_MULTILINE | wxTE_PROCESS_ENTER`) for input; Shift+Enter = newline
- [ ] `wxGauge` + `wxStaticText` in footer for context meter (green → yellow → red)
- [ ] LOCUS.md token cost shown as `[LOCUS.md: 120 tk]` chip in footer

### S1.4 — Tool Approval UI
- [ ] `ToolApprovalPanel : wxPanel` — dynamic show/hide between chat and input
- [ ] `wxStyledTextCtrl` for JSON args display (Scintilla JSON lexer, syntax-colored)
- [ ] `wxStaticText` for tool name badge + `preview()` text
- [ ] Buttons: Approve (Enter), Modify (M), Reject (Esc) with `wxAcceleratorTable`
- [ ] Modify flow: switch args STC to editable, parse JSON on confirm
- [ ] Post-execution: tool result injected into WebView as collapsible `<details>` block

### S1.5 — Context Compaction UI
- [ ] `on_compaction_needed` → modal `wxDialog` overlaid on chat
- [ ] Strategy radio buttons with description text
- [ ] Strategy A: trigger LLM summarisation → show proposed summary in scrollable text area → Confirm / Revise
- [ ] Strategy C: `wxSlider` for N turns + `wxListBox` live preview of which messages will be removed
- [ ] All strategies: show before/after token counts via `wxStaticText`, updated on strategy change
- [ ] Manual compaction: button in context meter footer area

### S1.6 — Workspace, File Tree & Settings
- [ ] `wxTreeCtrl` + `wxImageList` in left AUI pane for file tree; populated from `IndexQuery::list_directory()`
- [ ] Workspace open: `wxDirDialog` (native folder picker)
- [ ] Index status: `wxStaticText` labels (file counts, last indexed), `wxGauge` for active re-index
- [ ] Settings panel: `wxTextCtrl` for endpoint URL/model, `wxSpinCtrlDouble` for temperature, `wxSpinCtrl` for context limit, `wxTextCtrl` (multiline) for exclude patterns
- [ ] Workspace config saved to `.locus/config.json` on change

### S1.7 — Web Retrieval (RAG)
- [ ] `gumbo` vcpkg dependency added; HTML → plain text extractor (skip script/style/nav/footer)
- [ ] `web_pages`, `web_fts`, `web_headings` tables in index.db schema
- [ ] `WebSearchTool`: call configurable search API (Brave default), return titles + URLs + snippets
- [ ] `WebFetchTool`: HTTP GET via cpr → gumbo extract → store in web_fts → return outline only
- [ ] `WebReadTool`: read a section of a fetched page by heading or line range (like `read_file` for web)
- [ ] `search_text` extended: optional `sources` param to include web_fts alongside files_fts
- [ ] Web cache: per-session scoping, TTL eviction, configurable size cap (default 50 MB)
- [ ] Web config in `.locus/config.json`: enabled, provider, api_key, api_url, cache settings
- [ ] Per-workspace toggle: `web.enabled` (default false)
- [ ] Catch2 tests: HTML extraction, web_fts indexing, cache eviction

---

## M2 — Full Workspace Support

**Goal**: All three test workspaces functional. Semantic search operational.
Wikipedia (WS2) and personal documents (WS3) work end-to-end.

### S2.1 — Semantic Search
- [ ] ONNX Runtime C++ API initialised; model loaded from `models/all-MiniLM-L6-v2.onnx` (bundled)
- [ ] Bundle INT8 quantized `all-MiniLM-L6-v2.onnx` (~23 MB, Apache 2.0) in release; add `THIRD_PARTY_LICENSES`
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
- [ ] UI: visible "attached context" chip above input showing file + line; click to clear
- [ ] Standalone attach: user can right-click a file in the file tree panel → "Attach to context"

---

## M3 — Connected

**Goal**: Core accessible over LAN. VS Code sends editor context. Browser frontend works.

### S3.1 — CrowServer Frontend
- [ ] `CrowFrontend : IFrontend` in `src/frontends/crow/` — registers with Core, serves external clients
- [ ] Crow server starts on Core init, binds to `127.0.0.1:PORT` (default 7700)
- [ ] WebSocket: translates Core callbacks → JSON messages, routes incoming → `ILocusCore` calls
- [ ] HTTP REST: `GET /workspaces`, `POST /workspaces/open`, `GET /sessions`, `GET /status`
- [ ] Protocol version header on all responses; mismatch → `426 Upgrade Required`

### S3.2 — Remote Access
- [ ] Crow: optional bind to `0.0.0.0:PORT` (off by default; toggle in settings)
- [ ] Bearer token: generate random 32-byte token on first run, store in `.locus/auth.token`
- [ ] Token display: shown in settings panel with copy button; used by remote clients
- [ ] Auth middleware: skip for `127.0.0.1`; require `Authorization: Bearer <token>` for all others
- [ ] Self-signed TLS cert: generate with mbedTLS or similar on first LAN-mode enable, store in `.locus/`
- [ ] Trust-on-first-use: remote client shows cert fingerprint, user confirms once
- [ ] Tray icon tooltip: show active remote connections count

### S3.3 — VS Code Shim
- [ ] TypeScript VS Code extension project: `package.json`, `tsconfig.json`, `src/extension.ts`
- [ ] On `vscode.window.onDidChangeTextEditorSelection`: build `EditContext`, POST to Core `/edit-context`
- [ ] On `vscode.workspace.onDidChangeTextDocument`: throttle, update context
- [ ] Extension settings: `locus.coreUrl` (default `http://127.0.0.1:7700`), `locus.token`
- [ ] Status bar item: `$(locus-icon) Locus: Connected` / `Disconnected`; click to open settings
- [ ] Activation: on workspace open; deactivation: clean up on VS Code close

### S3.4 — Web / Browser Frontend
- [ ] Single HTML file + inline CSS + vanilla JS (no build step, no framework)
- [ ] WebSocket client: connect to CrowServer, handle all message types
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
- Native mobile app (CrowServer client)
- Plugin system for community tools
- Line-level diff viewer for AI-proposed file changes (wxStyledTextCtrl, dtl library)
- Voice chat
- Feeding an image as an inut to LLM

