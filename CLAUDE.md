# Locus — Claude Working Guide

This file is for Claude Code. It provides the context needed to work effectively
on this project without wasting tokens re-reading everything from scratch.

---

## Project in One Sentence

Locus is a workspace-bound local LLM agent assistant: one app instance = one folder.
The agent navigates the folder via a live file index, uses tools instead of dumping
data into context, and keeps the user in full transparent control of every step.

---

## Owner Profile

- **Background**: C++ / C# / Objective-C / shaders, 15+ years, primarily 3D graphics
- **Performance philosophy**: Extremely performance-conscious. Hates bloated software.
  Instant UI response is a hard requirement — no input lag, no phantom waits.
  Low memory and CPU usage matter. Think "good old software" standards.
- **Control style**: Wants full visibility into what AI is doing. Prefers step-by-step
  control over full automation. High-level thinker, expects AI to do the heavy lifting.
- **Anti-Electron**: Explicitly rejected. Any web-based UI must use OS-provided WebView,
  not a bundled Chromium.
- **Primary editor**: VS Code. Editor context integration (file/cursor/selection) is a
  planned v1.5 feature via a thin VS Code shim.
- **Starting platform**: Windows 11

**How to code for this user:**
- Don't add abstraction layers unless necessary
- Don't pad output with summaries of what was just done
- Prefer direct, efficient solutions — no speculative complexity
- When suggesting architecture, lean toward lean and explicit over framework-heavy
- Treat them as a senior dev — skip basics, go straight to substance

---

## Current Stage

**M2 — Full Workspace Support in progress.** S2.1 complete (semantic search).
See [roadmap.md](roadmap.md) for full status.

---

## Key Decisions Made

| Decision | Choice | Rationale |
|---|---|---|
| Project name | **Locus** | Unique, precise, workspace-centric |
| Workspace config dir | `.locus/` | Lives inside each workspace root |
| Deployment target | **Standalone app** | Independent of any editor, all use cases |
| Build sequence | **CLI prototype → desktop UI** | Validate hard parts before committing to UI |
| Core language | **C++20** | User's native language, zero runtime overhead |
| Build system | **CMake + vcpkg** | Standard C++ toolchain |
| Compiler | **MSVC 19.50 (VS 2026)** | Generator: `Visual Studio 18 2026` |
| Index database | **SQLite + FTS5** | Zero config, fast, native C API |
| Code parser | **Tree-sitter** | C library, polyglot, battle-tested |
| File watcher | **efsw** | Cross-platform wrapper (Win32/inotify/FSEvents) |
| HTTP / LLM client | **cpr + SSE parsing** | Clean C++ libcurl wrapper, streaming |
| LLM backend (v1) | **LM Studio** | OpenAI-compatible REST at localhost:1234 |
| Test model | **Gemma 4 26B A4B** | Modern mid-range consumer GPU model |
| Core deployment | **System tray daemon** | Headless always-on process, frontends attach via API |
| Frontend API | **Crow (WebSocket + HTTP)** | Any frontend, any language, including remote |
| Desktop UI (v1) | **wxWidgets + wxWebView** | Native widgets, OS-provided WebView2 for markdown, no bundled runtime |
| ZIM reader | **libzim** | Kiwix/Wikipedia native format |
| VS Code integration | **Thin shim, v1.5** | ~200 lines TS sends context over local socket |
| Logging | **spdlog** | File sink + stderr, structured levels |
| Threading | **std::thread + std::mutex/queue** | Explicit, no framework |
| Tests | **Catch2** | Unit tests from day one, separate CMake target |
| HTML parser | **gumbo-parser** | Google's C HTML5 parser, handles malformed HTML |
| Web search API | **Brave Search (default)** | Clean JSON API, free tier, configurable |
| License | **MIT** | Maximum permissive, community-friendly |

---

## Coding Conventions

Established patterns — follow these for consistency across the codebase.

**Namespace**: All project code lives in `namespace locus`.

**File layout**: `src/name.h` (header) + `src/name.cpp` (implementation). No `include/` directory —
headers and sources live together in `src/`.

**CMake structure**: Core code is a static library (`locus_core`). Both the main executable (`locus`)
and the test executable (`locus_tests`) link against it. New `.cpp` files go into `locus_core`'s
source list in the root `CMakeLists.txt`.

**Class style**: RAII wrappers for external resources (SQLite, efsw). Rule of five — delete
copy ops, no move unless needed. Use `std::unique_ptr` for owned subsystem pointers.

**Error handling**: `std::runtime_error` for fatal init failures (bad path, DB open fail).
Logged warnings for recoverable issues (missing LOCUS.md, unparseable config).

**Logging**: `spdlog::info` for stage transitions and lifecycle. `spdlog::trace` for SQL queries,
file events, and diagnostic detail (visible under `-verbose`). `spdlog::warn`/`error` for problems.

**Naming**: `snake_case` for files, functions, variables. `PascalCase` for classes and structs.
`snake_case` for struct members. Constants: `k_name` or `constexpr`.

**Tests**: Catch2. One `test_<topic>.cpp` per subsystem. Tag tests with stage: `[s0.2]`, `[s0.3]`.
Test names use ASCII only (no Unicode — CTest mangles it on Windows).

**Static CRT**: vcpkg triplet `x64-windows-static` with custom overlay at `cmake/triplets/`
that adds `-D_USE_STD_VECTOR_ALGORITHMS=0` to work around missing SIMD intrinsics in `libcpmt.lib`.
This flag must be set both in the triplet (for vcpkg deps) and in `CMakeLists.txt`
(for project code).

---

## Code Map

Source is in `src/`, tests in `tests/`, architecture docs in `architecture/`.
Core is a static lib (`locus_core`). Both `locus` (exe) and `locus_tests` link it.

| File | What it owns | Key types |
|---|---|---|
| `src/workspace.h/cpp` | Opens a folder, owns DB + watcher + indexer + query. One per folder. | `Workspace`, `WorkspaceConfig` |
| `src/database.h/cpp` | RAII SQLite wrapper. WAL mode. Schema creation. | `Database` |
| `src/file_watcher.h/cpp` | efsw wrapper. Debounced `FileEvent` queue. | `FileWatcher`, `FileEvent` |
| `src/indexer.h/cpp` | Initial traversal + incremental updates. Tree-sitter symbol parsing. Uses `ExtractorRegistry` for text/heading extraction. | `Indexer`, `Indexer::Stats` |
| `src/index_query.h/cpp` | Read-only queries: FTS5 search, symbol lookup, outlines, dir listing. | `IndexQuery`, `SearchResult`, `SymbolResult`, `OutlineEntry`, `FileEntry` |
| `src/llm_client.h/cpp` | SSE streaming to OpenAI-compatible endpoints. Token estimation. | `ILLMClient`, `ChatMessage`, `ToolCallRequest`, `LLMConfig`, `StreamCallbacks` |
| `src/sse_parser.h/cpp` | Low-level SSE `data:` line parser. | `SseParser` |
| `src/tool.h` | Tool system interfaces (no .cpp — pure abstract + structs). | `ITool`, `IToolRegistry`, `ToolParam`, `ToolResult`, `ToolCall`, `WorkspaceContext` |
| `src/tool_registry.h/cpp` | Concrete registry. Schema JSON builder. ToolCall parser. | `ToolRegistry` |
| `src/tools.h/cpp` | All 12 built-in tools + `register_builtin_tools()` factory. | `ReadFileTool`, `WriteFileTool`, `CreateFileTool`, `DeleteFileTool`, `ListDirectoryTool`, `SearchTextTool`, `SearchSymbolsTool`, `GetFileOutlineTool`, `RunCommandTool`, `AskUserTool`, `SearchSemanticTool`, `SearchHybridTool` |
| `src/embedder.h/cpp` | ONNX Runtime session wrapper. Loads model, runs inference, returns normalized embedding vector. | `Embedder` |
| `src/chunker.h/cpp` | Content chunking: code (Tree-sitter boundaries), document (heading boundaries), sliding window fallback. | `Chunk`, `SymbolSpan`, `chunk_code()`, `chunk_document()`, `chunk_sliding_window()` |
| `src/embedding_worker.h/cpp` | Background thread: consumes chunk queue, calls Embedder, writes embeddings to DB. | `EmbeddingWorker` |
| `src/extractors/text_extractor.h` | Base interface for file format extractors (no .cpp — pure abstract + structs). | `ITextExtractor`, `ExtractionResult`, `ExtractedHeading` |
| `src/extractors/extractor_registry.h/cpp` | Maps file extension → `ITextExtractor`. Owned by `Workspace`. | `ExtractorRegistry` |
| `src/extractors/markdown_extractor.h/cpp` | Extracts text + `#` headings from `.md` files. | `MarkdownExtractor` |
| `src/extractors/html_extractor.h/cpp` | Extracts text + `<h1>`–`<h6>` headings from `.html` files. | `HtmlExtractor` |
| `src/glob_match.h` | Header-only glob pattern matching for exclude patterns. | `glob_match()` |
| `src/frontend.h` | Frontend/core interfaces (no .cpp — pure abstract + enums). | `IFrontend`, `ILocusCore`, `ToolDecision`, `CompactionStrategy` |
| `src/frontend_registry.h` | Thread-safe frontend registry with exception-isolated fan-out. Header-only. | `FrontendRegistry` |
| `src/conversation.h/cpp` | Conversation history with JSON serialization and compaction. | `ConversationHistory` |
| `src/system_prompt.h/cpp` | Assembles system prompt from base + LOCUS.md + metadata + tools. | `SystemPromptBuilder`, `WorkspaceMetadata` |
| `src/session_manager.h/cpp` | Save/load/list conversation sessions to `.locus/sessions/`. | `SessionManager`, `SessionInfo` |
| `src/agent_core.h/cpp` | Agent loop on dedicated thread: LLM → stream → tool calls → approval → execute → resume. | `AgentCore` |
| `src/frontends/cli_frontend.h/cpp` | Terminal frontend: token streaming, y/n/e tool approval, context meter, compaction prompts. | `CliFrontend` |
| `src/main.cpp` | CLI entry point. Arg parsing, logging init, REPL loop, Ctrl+C handler. | `CliArgs` |
| `src/gui/locus_app.h/cpp` | wxApp entry point. Owns Workspace, LLM, Agent lifetime. Single-instance check. | `LocusApp` |
| `src/gui/locus_frame.h/cpp` | Main window. wxAuiManager 3-pane layout, menu bar, status bar. Binds agent events. | `LocusFrame` |
| `src/gui/chat_panel.h/cpp` | Chat UI: wxWebView (HTML/CSS), wxTextCtrl input, context meter footer. Streaming via wxTimer + md4c. | `ChatPanel` |
| `src/gui/markdown.h/cpp` | md4c wrapper: markdown → HTML (GitHub-flavored dialect). | `markdown_to_html()` |
| `src/gui/locus_tray.h/cpp` | System tray icon. State display (idle/active/error), right-click menu, minimize-to-tray. | `LocusTray` |
| `src/gui/tool_approval_panel.h/cpp` | Tool approval: AUI bottom pane with JSON args (Scintilla), approve/modify/reject buttons, ask_user mode. | `ToolApprovalPanel` |
| `src/gui/wx_frontend.h/cpp` | IFrontend thread bridge: agent thread callbacks → wxThreadEvent → UI thread. | `WxFrontend` |
| `src/gui/compaction_dialog.h/cpp` | Context compaction modal: strategy B/C radio, N-turns slider, message preview, before/after token counts. | `CompactionDialog`, `CompactionChoice` |
| `src/gui/file_tree_panel.h/cpp` | File tree sidebar: wxTreeCtrl with lazy-loading from IndexQuery, index stats, re-index gauge. | `FileTreePanel` |
| `src/gui/settings_dialog.h/cpp` | Settings modal: LLM endpoint/model/temperature/context, index exclude patterns. | `SettingsDialog` |
| `src/gui/autostart.h/cpp` | Windows startup-on-login via HKCU Run key (opt-in). | `is_autostart_enabled()`, `set_autostart_enabled()` |

**Test files** follow `tests/test_<topic>.cpp` — one per subsystem, tagged by stage.

**Data flow**: `main` → `Workspace` (owns `Database`, `ExtractorRegistry`, `FileWatcher`, `Indexer`, `IndexQuery`) → `AgentCore` (owns `ConversationHistory`, `SessionManager`, bridges `ILLMClient` + `IToolRegistry` + `WorkspaceContext`) → frontends receive events via `IFrontend` through `FrontendRegistry`.

**Adding a new file format extractor**: Create `src/extractors/<format>_extractor.h/cpp` implementing `ITextExtractor`. Register it in `Workspace::Workspace()` (`workspace.cpp`) with `extractors_->register_extractor(".ext", ...)`. Add the `.cpp` to `locus_core` in `CMakeLists.txt`. No changes to `indexer.cpp` needed.

---

## Document Map

| File | When to read |
|---|---|
| [CONTRIBUTING.md](CONTRIBUTING.md) | Code style, build/run/test instructions for humans |
| [roadmap.md](roadmap.md) | Full roadmap — milestones, stages, tasks. Current status of build. |
| [test-workspaces.md](test-workspaces.md) | Three concrete test cases — read when scoping features |
| [vision.md](vision.md) | Understanding the *why* behind any design decision |
| [DIFFERENTIATORS.md](DIFFERENTIATORS.md) | Who this is for, what makes it different |
| [requirements.md](requirements.md) | Full feature list — reference when scoping work |
| [architecture/overview.md](architecture/overview.md) | System component map, data flow, context strategy |
| [architecture/tool-protocol.md](architecture/tool-protocol.md) | ITool interface, approval flow, adding tools |
| [architecture/workspace-index.md](architecture/workspace-index.md) | Index subsystem: schema, update strategy, query API |
| [architecture/tech-stack.md](architecture/tech-stack.md) | Tech stack — decided choices and future frontend options |
| [architecture/web-retrieval.md](architecture/web-retrieval.md) | Web RAG: fetch → index → search pipeline, token budget strategy |

---

## Key Architecture Principles (Non-negotiable)

1. **Index is never written by the LLM** — only deterministic algorithmic updates
2. **Tools over context stuffing** — agent fetches what it needs, never pre-loads
3. **Paginate everything** — no full-file dumps into context
4. **Operating transparency** — User sees tool calls, LLM/context state and other operations - nothing executes without user seeing it, everything is transparent
5. **Context window is precious** — always summarize tool results before injection
6. **Instant UI feedback** — streaming responses, no blocking main thread ever

---

## Development & Testing Protocol

**Test workspace**: `d:\Projects\AICodeAss\` — this folder IS WS1. Locus always runs against
itself during development. What Locus indexes is exactly what is visible here.

**After every completed stage:**
1. Build the project
2. Run `locus.exe <workspace_path> -verbose`
3. Exercise the stage's features
4. Quit locus.exe
5. Read `.locus/locus.log` — verify expected behavior, catch errors
6. Fix issues before moving to the next stage

**`-verbose` flag** (implemented in S0.1):
- spdlog level drops to `trace` (vs default `info`)
- All SQL queries logged
- Every tool call logged with full args and result
- LLM token stream logged
- Index update events logged

This protocol means every stage is validated before the next begins.
No accumulated debt, no "we'll debug it later."

**After every completed stage/task — mandatory bookkeeping:**
1. Update `roadmap.md`: mark completed tasks `[x]`, add ✔ to the stage header
2. Update `CLAUDE.md` "Current Stage" line to reflect the new state
3. Do this immediately after verification passes, before moving on

---

## Token Efficiency Notes

When working on this project's own code:
- Read files before editing (read the specific section, not whole file)
- Use Grep for finding specific patterns before reading
- The architecture docs are the source of truth — don't re-derive what's written there
- This CLAUDE.md file should stay concise — it's a quick-scan reference, not a second vision doc
