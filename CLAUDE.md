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

**M4 — Agent Quality in progress.** S4.A / S4.P / S4.J / S4.B / S4.K / S4.I / S4.S / S4.M done. M3 refactoring complete — S3.C (`IWorkspaceServices`), S3.F (`WatcherPump` + `LocusFrame` split), S3.A (`AgentCore` split into `AgentLoop` / `ToolDispatcher` / `ActivityLog` / `ContextBudget`; agent sources moved to `src/agent/`), S3.J (`SlashCommandParser` + `SlashCommandDispatcher` extracted), S3.E (`tools.cpp` split into `src/tools/` by family — shared / file / search / index / process / interactive), S3.D (`Indexer` split into `TreeSitterRegistry` + `ISymbolExtractor` registry + `IndexerStatements` RAII; index sources moved to `src/index/` — see below), S3.K (`architecture/agent-loop.md`, `architecture/decisions/` ADR trail), S3.I (`architecture/threading-model.md` + `ConversationHistory::assert_owner_thread()` fence, `ConversationOwnerScope` RAII), S3.L (tool-catalog hygiene: unified `search(mode=…)`, `ITool::available()` / `visible_in_mode()` gating, per-turn manifest-token warning), S3.G (`LocusSession` bundle — see below) all done.

**S4.A (Diff-Based Editing) done.** One `edit_file` tool in [src/tools/file_tools.cpp](src/tools/file_tools.cpp): takes `{path, edits: [{old_string, new_string, replace_all?}, ...]}`. A single edit is an array of one; multiple edits apply atomically (all succeed or none are written) with later edits seeing earlier results. Uniqueness is enforced per edit. Writes go through temp-file + rename for atomicity. Process-wide `ReadTracker` in [src/tools/shared.h](src/tools/shared.h) forces a prior `read_file` on the same path before edits are accepted — mirrors Claude Code's hallucinated-edit mitigation. `create_file` folded into `write_file` with an `overwrite` flag (default `false`); refusing to replace an existing file is now the default, set `overwrite=true` explicitly to opt in. System prompt updated to prefer `edit_file` over `write_file`. Approval panel renders `edit_file` as a red/green unified-diff inside the existing Scintilla pane; `Modify` flips to raw JSON and back. Kept tool set slim (9 built-ins, not 11) to keep the manifest small for local LLMs.

**S4.P (Regex search) done.** New `mode=regex` on the unified `search` tool — in-process `std::regex` (ECMAScript) over every indexed, non-binary file, with `pattern` / `path_glob` / `case_sensitive` / `max_results` params. Line-numbered output with ±1 line context. File enumeration goes through `IndexQuery::list_directory("", huge_depth)` so the indexer's exclude patterns are already respected — no duplicate filtering. `SearchRegexTool` lives in [src/tools/search_tools.h/cpp](src/tools/search_tools.h) alongside the other per-mode classes; it isn't registered directly — `SearchTool::execute` delegates on `mode=regex`. `query` on the `search` tool is now optional at schema level; each mode enforces its own required field at runtime. Still 9 built-in tools.

**S3.D (`Indexer` split) done.** Tree-sitter language map and prepared statements lifted out of `Indexer`. New [src/index/tree_sitter_registry.h/cpp](src/index/tree_sitter_registry.h) owns the `TSParser*` and the `name → TSLanguage*` map (9 grammars); intended to be shared with S4.M `ast_search`. New [src/index/symbol_extractor.h/cpp](src/index/symbol_extractor.h) defines `ISymbolExtractor` returning `vector<ExtractedSymbol>` (kind/name/signature/parent_name/line span) plus a `SymbolExtractorRegistry`; the shared two-level walk + `extract_name_from_node` helper live on a private `RuleBasedSymbolExtractor`. Per-language `.cpp` files under [src/index/symbol_extractors/](src/index/symbol_extractors/) hold only their `SymbolRule` table and a `make_<lang>_symbol_extractor()` factory (cpp / python / js_ts / go / rust / java / csharp); `register_builtin_symbol_extractors` wires all seven languages into the `Indexer`'s registry. New [src/index/prepared_statements.h/cpp](src/index/prepared_statements.h) `IndexerStatements` RAII holder consolidates all 14 sqlite_stmt pointers (10 main.db + 4 vectors.db) into one ctor/dtor pair — `Indexer::~Indexer()` is now a one-liner trace log. Existing `indexer.{h,cpp}` / `index_query.{h,cpp}` / `chunker.{h,cpp}` / `glob_match.h` moved into `src/index/`; all 17 includers updated. DB schema unchanged; WS1 cold-reindex matches pre-refactor totals (284 files / 1394 symbols / 612 headings). See [roadmap/M3/S3.D-indexer-split.md](roadmap/M3/S3.D-indexer-split.md).

**S3.G (`LocusSession` bundle) done.** New [src/core/locus_session.h/cpp](src/core/locus_session.h) owns the `Workspace` + `ILLMClient` + `ToolRegistry` + `AgentCore` lifecycle as a single object. Constructor wires them in order (workspace → LLM client → server probe → tools → agent_core, then `agent->start()`); destructor calls `agent_->stop()` first so the agent thread joins before any `unique_ptr` member is freed, then unwinds in reverse declaration order. LLM-config layering happens in one place: defaults → `.locus/config.json` → caller seed (CLI / GUI args) → live `query_model_info`. The CLI now respects `.locus/config.json` like the GUI always has. `main.cpp` shrunk ~70 lines and `LocusApp` shrunk ~120 lines (workspace switch is `frame_->Destroy(); session_.reset(); spawn_session(...)`); future M4 subsystems (LSP, MCP, process registry) slot in as more `unique_ptr` members on `LocusSession` with zero frontend changes.

**S4.B (Checkpoint & Undo) done.** Pre-mutation snapshots for `edit_file` / `write_file` / `delete_file` land under `.locus/checkpoints/<session_id>/<turn_id>/` (manifest in `checkpoint.json`, byte-for-byte file copies in `files/<rel_path>`). New [src/agent/checkpoint_store.h/cpp](src/agent/checkpoint_store.h) owns the on-disk layout; `ToolDispatcher::set_turn_context` arms snapshotting per agent turn (slash-command direct dispatch deliberately bypasses — explicit user action). `AgentCore` generates a process-lifetime session id at construction, bumps a turn counter on each user message, and drops the empty turn dir at end-of-turn so `/undo` doesn't see phantom turns. New `ILocusCore::undo_turn(turn_id=0)` (0 → most recent), exposed in the GUI as a chat-footer **Undo** button and on every frontend as the `/undo [turn_id]` slash command. GC keeps the most-recent 50 turns per session (configurable); `reset_conversation` drops the whole checkpoint session and rolls a fresh id. 1 MB per-file cap — oversized files are recorded in the manifest with `skipped=true` so the user sees them in the restore summary and reaches for git instead. Saved conversation sessions are deliberately decoupled from checkpoint sessions (different identities, different lifetimes) — see [roadmap/M4/S4.B-checkpoint-undo.md](roadmap/M4/S4.B-checkpoint-undo.md) for the rationale.

**S4.S (Telemetry & Agent Performance Metrics) done.** New thread-safe [src/agent/metrics.h/cpp](src/agent/metrics.h) `MetricsAggregator` owned by `AgentCore` next to `ActivityLog` / `ContextBudget`. Per-turn samples + per-tool histogram + retrieval hit rate; `AgentLoop` records `(usage, stream_ms)` per LLM step, `ToolDispatcher` records `(name, success, ms, content_size)` per call, and parses retrieval result counts out of search-tool result headers via `parse_search_result_count`. Saved-session JSON now carries a top-level `"metrics"` key (`SessionManager::save` grew an optional `extras` parameter). Two built-in slash commands handled directly in `AgentCore::try_slash_command`: `/metrics` (one-shot text summary) and `/export_metrics [json|csv]` (writes to `.locus/metrics/`). GUI: `ActivityPanel` is now a 2-tab `wxNotebook` (Log + Metrics); the new [src/gui/metrics_view.h/cpp](src/gui/metrics_view.h) `MetricsView` renders the aggregates + two `wxDC` spark bars (turn duration, total tokens) with no external charting lib, refreshes on timer + per-event, and offers "Export JSON…/CSV…" file-dialog buttons. Tool catalog unchanged (still 13). 12 new unit tests in [tests/test_metrics.cpp](tests/test_metrics.cpp) tagged `[s4.s][metrics]`.

**S4.I (Background / Long-Running Commands) done.** New [src/process_registry.h/cpp](src/process_registry.h) owns a per-workspace map of `BackgroundProcess` instances; each child is spawned under a Win32 Job Object (`KILL_ON_JOB_CLOSE`) so terminating the registry kills the whole tree (including grandchildren `cmd /c` spawns). One reader thread per process drains stdout+stderr into a `std::string` capped at `WorkspaceConfig::process_output_buffer_kb` (default 256 KB); monotonic `total_bytes_received` + `bytes_dropped` let `read_output(since_offset?)` tell the LLM exactly how many bytes the ring evicted. Four new tools live in [src/tools/process_tools.h/cpp](src/tools/process_tools.h) — `run_command_bg`, `read_process_output`, `stop_process`, `list_processes` — gating themselves off (`available()=false`) when the workspace has no registry. `IWorkspaceServices::processes()` is the access point; `Workspace` owns the registry and tears it down before the watcher pump so background processes can't outlive the workspace. Tool count rose from 9 to 13. The system prompt now distinguishes synchronous `run_command` (≤30s) from `run_command_bg` (servers, long builds, watchers). UI panel + tray badge deferred — tool calls already surface in the existing activity log + chat. See [roadmap/M4/S4.I-background-commands.md](roadmap/M4/S4.I-background-commands.md).

**S4.K (Retrieval Evaluation Harness) done.** New manual-only [`locus_retrieval_eval`](tests/retrieval_eval/main.cpp) binary opens a workspace, waits for the embedding queue to drain, and runs every gold query in [tests/retrieval_eval/queries.json](tests/retrieval_eval/queries.json) (80 entries against WS1 itself) through `search_text` / `search_semantic` / `search_hybrid`, computing recall@{1,3,5,10}, MRR, and nDCG@10. Aggregate + per-query report goes to [tests/retrieval_eval/results.md](tests/retrieval_eval/results.md); baseline lives at `tests/retrieval_eval/baseline.json` (committed via `--write-baseline`). On normal runs, exit code 1 fires when any metric drops more than `--tolerance` (default 5%) vs. the baseline. **Not wired into `ctest`** — the harness needs a fully indexed workspace + bge-m3 (~600 MB) + queue drain (tens of seconds), too heavy for "every build". Pure-math metric tests in [tests/test_retrieval_eval.cpp](tests/test_retrieval_eval.cpp) DO run with the rest of the suite. Activity-log replay is wired as `--curate-from <sessions_dir>` ([tests/retrieval_eval/curate.cpp](tests/retrieval_eval/curate.cpp)) — walks `.locus/sessions/*.json`, extracts every search-tool invocation from saved conversations, prints a `queries.json` template with empty `expected_files` arrays for hand-filling. See [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md) for the run protocol.

**S4.M (Tree-sitter structural search) done.** New `mode=ast` on the unified `search` tool. Caller supplies `language` (one of the nine grammars in `TreeSitterRegistry`) and a Tree-sitter S-expression `query` (e.g. `(call_expression function: (identifier) @fn (#eq? @fn "malloc"))`); optional `path_glob` and `capture` (filter to a single capture name). `SearchAstTool` lives in [src/tools/search_tools.h/cpp](src/tools/search_tools.h) alongside the other per-mode classes; not registered as a top-level tool — `SearchTool::execute` delegates on `mode=ast`. Each call constructs its own short-lived `TreeSitterRegistry` (cheap, sidesteps locking against the indexer's parser). Tree-sitter does NOT auto-enforce `#eq?` / `#match?` / `#any-of?` predicates — `match_passes_predicates()` walks `ts_query_predicates_for_pattern` steps and applies `eq?`/`not-eq?`/`match?`/`not-match?`/`any-of?`/`not-any-of?` against capture text. Output: `path:line  @capture  inline_text` plus a 2-line snippet, with `[truncated at max_results]` when capped. Curated reference query library under [queries/](queries/) (cpp / python / typescript / rust / go) — copy-paste starters for the LLM, not auto-loaded. Tool catalog unchanged (13). 13 new unit tests in [tests/test_search_ast.cpp](tests/test_search_ast.cpp) tagged `[s4.m][search][ast]`. The "rewrite `get_file_outline` on `.scm` queries" task from the stage spec was deferred — the rule-based extractors just landed via S3.D and a parallel `.scm` rewrite would have no user-visible effect.

**S4.J (Embeddings + Reranker) done.** Default embedder swapped from `all-MiniLM-L6-v2` (384-dim, English) to `bge-m3` (1024-dim, multilingual, 8K-capable). Embedding dimension is read from the GGUF (`llama_model_n_embd`) rather than configured — `WorkspaceConfig::embedding_dimensions` removed. New `Database::ensure_vectors_schema(dim)` + `meta` table in `vectors.db` detect dim mismatch on workspace open, drop `chunks` + `chunk_vectors`, and let the indexer re-chunk + re-enqueue every file (full re-embed). New [src/reranker.h/cpp](src/reranker.h) wraps llama.cpp with `LLAMA_POOLING_TYPE_RANK` for `bge-reranker-v2-m3`; loaded by `Workspace` when `reranker_enabled` is true and exposed through `IWorkspaceServices::reranker()`. `search_semantic` and `search_hybrid` now pull `reranker_top_k` candidates (default **20** — see stage doc for the perf reality) and re-score them with the cross-encoder before trimming to `max_results`. Reranker is **off by default** (opt-in via Settings → Index, or `config.json`). New [models/download.ps1](models/download.ps1) + [models/download-small.ps1](models/download-small.ps1) fetch the GGUFs (see [README.md](README.md#models)). Original "<200ms for 50 candidates" target was off by ~25× on bge-reranker-v2-m3 (568M params); the test now guards against catastrophic regressions only — see [roadmap/M4/S4.J-embeddings-reranker.md](roadmap/M4/S4.J-embeddings-reranker.md) for the full numbers.

See [roadmap/M4/](roadmap/M4/) for remaining stages.

**M3 is now Refactoring** (not Agent Quality). Old M3 → M4 (Agent Quality), old M4 → M5 (Connected). Per-stage docs live under [roadmap/M3/](roadmap/M3/), [roadmap/M4/](roadmap/M4/), [roadmap/M5/](roadmap/M5/). [roadmap.md](roadmap.md) is the index.

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
| Index database | **SQLite + FTS5**, split into `.locus/index.db` (skeleton) + `.locus/vectors.db` (semantic, optional) | Zero config, native C API. Separate files + separate connections = independent WALs, so background embedding never blocks FTS/symbol writes. vectors.db is a detachable cache — delete it to reclaim disk or force re-embed. |
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
| HTML parser | **gumbo-parser** | Google's C HTML5 parser, handles malformed HTML (planned for S4.1 web RAG; S2.3 uses a regex-based stripper) |
| PDF extractor | **PDFium (bblanchon prebuilt, BSD-3-Clause)** | Fetched via `FetchContent` — not in vcpkg mainline. Ships as `pdfium.dll` alongside `locus.exe` |
| DOCX/XLSX reader | **miniz + pugixml** | miniz unzips OOXML package; pugixml walks `word/document.xml` / sheet XML |
| Index embedder | **llama.cpp (GGUF)** | CPU-only in-process inference; WordPiece vocab bundled in the GGUF; replaced ONNX Runtime which had MSVC-static-CRT schema-registration issues |
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
Test names use ASCII only (no Unicode - CTest mangles it on Windows).

**No em-dashes anywhere.** Use ASCII `-` (or `--`) in code, comments, commit messages, docs,
test names, and log strings. Em-dashes (`—`, U+2014) break ctest test-name dispatch on Windows
(no CP437 mapping) and complicate grep/diff. This rule applies to anything written into the
repo, including this file.

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
| `src/core/locus_session.h/cpp` | Single-ctor/dtor bundle of Workspace + ILLMClient + ToolRegistry + AgentCore (S3.G). Ctor handles wiring order + LLM-config layering (defaults → `.locus/config.json` → caller seed → live `query_model_info`); dtor stops the agent thread first, then unwinds in reverse declaration order. Both CLI (`main.cpp`) and GUI (`LocusApp`) construct one of these per open workspace; future lifecycle subsystems (LSP, MCP) plug in here. | `LocusSession` |
| `src/workspace.h/cpp` | Opens a folder, owns main_db (always) + vectors_db (optional) + watcher + indexer + query + watcher_pump. One per folder. | `Workspace`, `WorkspaceConfig` |
| `src/database.h/cpp` | RAII SQLite wrapper. WAL mode. Two schema kinds: `Main` (files/fts/symbols/headings) and `Vectors` (chunks + `meta` + vec0 chunk_vectors; loads sqlite-vec only here). `chunk_vectors` is created lazily by `ensure_vectors_schema(dim)`, which also performs the dim-mismatch wipe + re-embed migration. Includes legacy-table migration from the pre-S3 split. | `Database`, `DbKind` |
| `src/file_watcher.h/cpp` | efsw wrapper. Debounced `FileEvent` queue. | `FileWatcher`, `FileEvent` |
| `src/index/indexer.h/cpp` | Initial traversal + incremental updates. Owns `IndexerStatements` + `TreeSitterRegistry` + `SymbolExtractorRegistry`; uses `ExtractorRegistry` for text/heading extraction. Writes skeleton to main_db, chunks to vectors_db — transactions open on both connections in parallel. After S3.D, `index_file` is purely orchestration: read → extract → upsert → symbols → chunk → persist. | `Indexer`, `Indexer::Stats` |
| `src/index/index_query.h/cpp` | Read-only queries: FTS5 search, symbol lookup, outlines, dir listing. Semantic search is a two-step read — vectors_db for top-K chunks, main_db to resolve file_id → path. | `IndexQuery`, `SearchResult`, `SymbolResult`, `OutlineEntry`, `FileEntry` |
| `src/index/tree_sitter_registry.h/cpp` | Owns the `TSParser*` and the `name → TSLanguage*` map (9 grammars). `Indexer` holds one as a member; designed to be shared with S4.M `ast_search`. Caller must serialise parser use. | `TreeSitterRegistry` |
| `src/index/symbol_extractor.h/cpp` | `ISymbolExtractor` interface returning `vector<ExtractedSymbol>`, plus a `SymbolExtractorRegistry` keyed by language name. The shared two-level walk + `extract_name_from_node` helper live on a private `RuleBasedSymbolExtractor` inside the .cpp. `register_builtin_symbol_extractors()` wires the seven languages into the supplied registry. | `ISymbolExtractor`, `ExtractedSymbol`, `SymbolRule`, `SymbolExtractorRegistry` |
| `src/index/symbol_extractors/<lang>_extractor.cpp` | One file per language (cpp / python / js_ts / go / rust / java / csharp). Each holds only its `SymbolRule` table and a `make_<lang>_symbol_extractor()` factory that forwards to `make_rule_based_symbol_extractor`. No DB or Tree-sitter walking code. | — |
| `src/index/prepared_statements.h/cpp` | RAII holder for the 14 indexer prepared statements (10 main.db + 4 vectors.db; the vectors-db ones stay null when semantic search is disabled). ctor prepares everything, dtor finalises. Members are public so `Indexer` accesses them as `stmts_.upsert_file` etc. | `IndexerStatements` |
| `src/index/chunker.h/cpp` | Content chunking: code (Tree-sitter boundaries), document (heading boundaries), sliding window fallback. | `Chunk`, `SymbolSpan`, `chunk_code()`, `chunk_document()`, `chunk_sliding_window()` |
| `src/index/glob_match.h` | Minimal glob matcher (`*`, `**`, `?`) used by the indexer's exclude patterns and the regex-search tool. Header-only. | `glob_match()` |
| `src/llm_client.h/cpp` | SSE streaming to OpenAI-compatible endpoints. Token estimation. | `ILLMClient`, `ChatMessage`, `ToolCallRequest`, `LLMConfig`, `StreamCallbacks` |
| `src/sse_parser.h/cpp` | Low-level SSE `data:` line parser. | `SseParser` |
| `src/core/workspace_services.h` | `IWorkspaceServices` interface — tool-facing surface over a workspace (no .cpp — pure abstract). `Workspace` implements it; tests use `tests/support/fake_workspace_services.h`. | `IWorkspaceServices` |
| `src/core/watcher_pump.h/cpp` | Background thread that drains FileWatcher, batches by quiet-period (1.5s) + hard-cap (20s), calls `Indexer::process_events`. Owned by `Workspace` — replaces the pump that used to live duplicated in main.cpp / locus_app.cpp / locus_frame.cpp. Provides `flush_now()` for synchronous external flush. | `WatcherPump` |
| `src/core/workspace_lock.h/cpp` | RAII exclusive file lock on `.locus/locus.lock`. Acquired first in `Workspace` ctor, released last in dtor. Refuses to open when the same workspace — or any ancestor workspace — is already held by another Locus process. OS releases the lock on crash. Replaces the process-global `wxSingleInstanceChecker` in the GUI so two different workspaces can run side by side. | `WorkspaceLock` |
| `src/tool.h` | Tool system interfaces (no .cpp — pure abstract + structs). | `ITool`, `IToolRegistry`, `ToolParam`, `ToolResult`, `ToolCall` |
| `src/tool_registry.h/cpp` | Concrete registry. Schema JSON builder. ToolCall parser. | `ToolRegistry` |
| `src/tools/tools.h/cpp` | Aggregator header (re-exports every tool) + `register_builtin_tools()` factory. Individual tools live in the family headers below. | `register_builtin_tools` |
| `src/tools/shared.h/cpp` | `resolve_path` / `make_relative` / `error_result` helpers shared by tool implementations. Namespace `locus::tools`. | — |
| `src/tools/file_tools.h/cpp` | File CRUD + exact-string-diff edit tool (`EditFileTool` takes an `edits[]` array — single edit = array of one, batch = N items applied atomically). `WriteFileTool` subsumes the old `CreateFileTool` via an `overwrite` flag (default `false` refuses to replace existing files). | `ReadFileTool`, `WriteFileTool`, `EditFileTool`, `DeleteFileTool` |
| `src/tools/search_tools.h/cpp` | Unified `SearchTool` (mode: text/regex/symbols/semantic/hybrid) — the only search tool registered in the built-in manifest. The per-mode `Search*Tool` classes remain as internal implementations the dispatcher delegates to. | `SearchTool`, `SearchTextTool`, `SearchRegexTool`, `SearchSymbolsTool`, `SearchSemanticTool`, `SearchHybridTool` |
| `src/tools/index_tools.h/cpp` | Index-sourced listing and outline tools. | `ListDirectoryTool`, `GetFileOutlineTool` |
| `src/tools/process_tools.h/cpp` | Synchronous shell execution + S4.I background-process tools (`run_command_bg`, `read_process_output`, `stop_process`, `list_processes`). The bg tools mark `available()=false` when the workspace has no `ProcessRegistry`. | `RunCommandTool`, `RunCommandBgTool`, `ReadProcessOutputTool`, `StopProcessTool`, `ListProcessesTool` |
| `src/process_registry.h/cpp` | Per-workspace registry of long-running shell processes (S4.I). Each child runs under a Win32 Job Object (`KILL_ON_JOB_CLOSE`) so grandchildren spawned via `cmd /c` are killed with the parent; one reader thread per process drains stdout+stderr into a ring buffer capped at `WorkspaceConfig::process_output_buffer_kb`. The registry tracks per-pid last-delivered offset so `read_output(since_offset?)` can default to "everything new since last read". `Workspace` owns the registry and the dtor terminates every still-running child before the workspace closes. | `ProcessRegistry`, `BackgroundProcess` |
| `src/tools/interactive_tools.h/cpp` | Tools that go through the approval gate to solicit user input. | `AskUserTool` |
| `src/embedder.h/cpp` | llama.cpp session wrapper. Loads a GGUF model, tokenises via the embedded vocab, runs inference, returns an L2-normalised embedding vector. Dimension is read from the GGUF (`llama_model_n_embd`) and exposed via `dimensions()`; n_ctx defaults to 1024 (clamped to `n_ctx_train`). Workspace owns it as a `shared_ptr` so a process-wide `Workspace::set_embedder_provider` hook (used by the test suite) can hand the same instance to many Workspaces — single-Workspace production is unaffected. | `Embedder` |
| `src/reranker.h/cpp` | llama.cpp wrapper for cross-encoder GGUFs (e.g. `bge-reranker-v2-m3`) with `LLAMA_POOLING_TYPE_RANK`. `score(query, passage)` returns a single relevance logit; `score_batch()` is a convenience over many passages. Used by `search_semantic` / `search_hybrid` when `WorkspaceConfig::reranker_enabled` is true. | `Reranker` |
| `src/embedding_worker.h/cpp` | Background thread: consumes chunk queue, calls Embedder, writes embeddings to vectors_db (its own connection — no lock contention with the indexer writing to main_db). | `EmbeddingWorker` |
| `src/extractors/text_extractor.h` | Base interface for file format extractors (no .cpp — pure abstract + structs). | `ITextExtractor`, `ExtractionResult`, `ExtractedHeading` |
| `src/extractors/extractor_registry.h/cpp` | Maps file extension → `ITextExtractor`. Owned by `Workspace`. | `ExtractorRegistry` |
| `src/extractors/markdown_extractor.h/cpp` | Extracts text + `#` headings from `.md` files. | `MarkdownExtractor` |
| `src/extractors/html_extractor.h/cpp` | Extracts text + `<h1>`–`<h6>` headings from `.html` files. Strips `<script>`/`<style>`, decodes entities. | `HtmlExtractor` |
| `src/extractors/pdf_extractor.h/cpp` | Extracts text from `.pdf` files via PDFium. One pseudo-heading per page. Flags encrypted PDFs. | `PdfiumExtractor` |
| `src/extractors/docx_extractor.h/cpp` | Extracts text + `HeadingN` headings from `.docx` (unzip + parse `word/document.xml`). | `DocxExtractor` |
| `src/extractors/xlsx_extractor.h/cpp` | Extracts text from `.xlsx` sheets (resolves shared strings, emits one heading per sheet). | `XlsxExtractor` |
| `src/extractors/zip_reader.h/cpp` | miniz helper: read a single named entry from a .zip (shared by docx + xlsx). | `read_zip_entry()` |
| `src/frontend.h` | Frontend/core interfaces (no .cpp — pure abstract + enums). | `IFrontend`, `ILocusCore`, `ToolDecision`, `CompactionStrategy` |
| `src/frontend_registry.h` | Thread-safe frontend registry with exception-isolated fan-out. Header-only. | `FrontendRegistry` |
| `src/agent/conversation.h/cpp` | Conversation history with JSON serialization and compaction. | `ConversationHistory` |
| `src/agent/system_prompt.h/cpp` | Assembles system prompt from base + LOCUS.md + metadata + tools. | `SystemPromptBuilder`, `WorkspaceMetadata` |
| `src/agent/session_manager.h/cpp` | Save/load/list conversation sessions to `.locus/sessions/`. | `SessionManager`, `SessionInfo` |
| `src/agent/checkpoint_store.h/cpp` | Pre-mutation file snapshots for turn-level undo (S4.B). On-disk layout `.locus/checkpoints/<session_id>/<turn_id>/{checkpoint.json, files/<rel_path>}`. Idempotent within a turn, GC keeps last N turns, oversized files (>1 MB) get a manifest entry but no body. | `CheckpointStore`, `CheckpointEntry`, `TurnInfo`, `RestoreResult` |
| `src/agent/activity_log.h/cpp` | Thread-safe activity ring buffer: assigns monotonic ids, broadcasts each event via `FrontendRegistry`, and serves `get_since()` for late-joining frontends. Extracted from `AgentCore` in S3.A. | `ActivityLog` |
| `src/agent/context_budget.h/cpp` | Token accounting + overflow policy. Tracks last server-reported usage and previous-turn total for delta reporting; fires `on_compaction_needed` at the 80% soft threshold and 100% hard limit. Extracted from `AgentCore` in S3.A. | `ContextBudget` |
| `src/agent/metrics.h/cpp` | Thread-safe agent telemetry (S4.S). Per-turn samples, per-tool histogram, retrieval hit rate. `AgentLoop` records `(usage, stream_ms)` per LLM step; `ToolDispatcher` records `(name, ok, ms, content_size)` per call and parses retrieval result counts via `parse_search_result_count`. `to_json()` rides as `"metrics"` in saved sessions; `to_csv()` powers the GUI exporter and the `/export_metrics` slash. | `MetricsAggregator`, `TurnSample`, `ToolStat`, `RetrievalStat` |
| `src/agent/tool_dispatcher.h/cpp` | Tool approval gate + execute + result injection. Resolves per-workspace approval policy, waits on the decision condvar, and writes the tool-result message through an `AppendFn` so `AgentCore` stays the sole writer of `ConversationHistory`. S4.B: `set_turn_context()` arms a per-turn `CheckpointStore` so `edit_file`/`write_file`/`delete_file` snapshot before execute. Extracted from `AgentCore` in S3.A. | `ToolDispatcher` |
| `src/agent/agent_loop.h/cpp` | One LLM round trip: builds the tool schema, streams tokens/reasoning/tool-call fragments, emits the `llm_response` activity event, returns a parsed `AgentStepResult` for the orchestrator. No knowledge of tool execution. Extracted from `AgentCore` in S3.A. | `AgentLoop`, `AgentStepResult` |
| `src/agent/slash_commands.h/cpp` | Slash-command tokenizer + dispatcher. Parser is pure (quoted args, key=value, positional mapping, typed coercion via `std::from_chars`; raises `SlashParseError` on malformed input). Dispatcher owns execution, `/help` rendering, and `complete(prefix)` for autocomplete. Extracted from `AgentCore` in S3.J. | `SlashCommandParser`, `SlashCommandDispatcher`, `ParsedSlashCall`, `SlashCompletion`, `SlashParseError` |
| `src/agent/agent_core.h/cpp` | Orchestrator: agent thread + message queue + lifecycle, composing `AgentLoop` / `ToolDispatcher` / `ActivityLog` / `ContextBudget` / `SlashCommandDispatcher`. Owns `ConversationHistory` (only writer) and the attached-context state. | `AgentCore` |
| `src/frontends/cli_frontend.h/cpp` | Terminal frontend: token streaming, y/n/e tool approval, context meter, compaction prompts. | `CliFrontend` |
| `src/main.cpp` | CLI entry point. Arg parsing, logging init, REPL loop, Ctrl+C handler. | `CliArgs` |
| `src/gui/locus_app.h/cpp` | wxApp entry point. Owns Workspace, LLM, Agent lifetime. Single-instance check. | `LocusApp` |
| `src/gui/locus_frame.h/cpp` | Main window. wxAuiManager 3-pane layout, status bar, agent-event routing. Delegates menu bar to `MenuController`, status-pane composition to `OpsStatusView`. | `LocusFrame` |
| `src/gui/menu_controller.h/cpp` | Owns the wxMenuBar and its dynamic submenus (Recent Workspaces, Saved Sessions). All actions run through `Hooks` callbacks supplied by `LocusFrame`. | `MenuController` |
| `src/gui/ops_status_view.h/cpp` | Composes the right-pane status text from indexing/embedding progress counters. No wx dependencies beyond wxString. | `OpsStatusView` |
| `src/gui/chat_panel.h/cpp` | Chat UI: wxWebView (HTML/CSS), wxTextCtrl input, context meter footer. Streaming via wxTimer + md4c. | `ChatPanel` |
| `src/gui/markdown.h/cpp` | md4c wrapper: markdown → HTML (GitHub-flavored dialect). | `markdown_to_html()` |
| `src/gui/locus_tray.h/cpp` | System tray icon. State display (idle/active/error), right-click menu, minimize-to-tray. | `LocusTray` |
| `src/gui/tool_approval_panel.h/cpp` | Tool approval: AUI bottom pane with JSON args (Scintilla), approve/modify/reject buttons, ask_user mode. | `ToolApprovalPanel` |
| `src/gui/wx_frontend.h/cpp` | IFrontend thread bridge: agent thread callbacks → wxThreadEvent → UI thread. | `WxFrontend` |
| `src/gui/metrics_view.h/cpp` | "Metrics" tab inside `ActivityPanel` (S4.S). Renders `MetricsAggregator::aggregates()` — text totals plus two pure-`wxDC` spark bars (turn duration, total tokens). Refreshes on a 1 s timer + on every appended activity event. "Export JSON…/CSV…" buttons pop a `wxFileDialog` so the user picks the destination. | `MetricsView` |
| `src/gui/compaction_dialog.h/cpp` | Context compaction modal: strategy B/C radio, N-turns slider, message preview, before/after token counts. | `CompactionDialog`, `CompactionChoice` |
| `src/gui/file_tree_panel.h/cpp` | File tree sidebar: wxTreeCtrl with lazy-loading from IndexQuery, index stats, re-index gauge. | `FileTreePanel` |
| `src/gui/settings_dialog.h/cpp` | Settings modal: LLM endpoint/model/temperature/context, index exclude patterns. | `SettingsDialog` |
| `src/gui/autostart.h/cpp` | Windows startup-on-login via HKCU Run key (opt-in). | `is_autostart_enabled()`, `set_autostart_enabled()` |

**Test files** follow `tests/test_<topic>.cpp` — one per subsystem, tagged by stage. `tests/support/shared_embedder.h/cpp` lazily loads `bge-small-en-v1.5-Q8_0.gguf` once per process and installs a `Workspace::set_embedder_provider` hook via a Catch2 EventListener; the listener clears the hook + drops the cached model at `testRunEnded`. Without this the suite reloaded bge-m3 (~6 s × 38 Workspace-creating tests = ~240 s); with it the full suite runs in ~21 s.

**Integration tests** live in `tests/integration/` — a separate `locus_integration_tests` executable that drives `AgentCore` against a live LM Studio LLM end-to-end. Manual-only (not in `ctest`). See [tests/integration/README.md](tests/integration/README.md) for what each tag area covers.

**Retrieval eval harness (S4.K)** lives in `tests/retrieval_eval/` — a separate `locus_retrieval_eval` executable that opens a workspace, runs every gold query in `queries.json` through `search_text` / `search_semantic` / `search_hybrid`, and emits recall@K + MRR + nDCG@10 to `results.md`. Manual-only (full index + bge-m3 + queue drain is too heavy for `ctest`); pure-math metric tests in [tests/test_retrieval_eval.cpp](tests/test_retrieval_eval.cpp) DO run with the rest of the suite. See [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md).

**Data flow**: `main` → `Workspace` (owns `Database`, `ExtractorRegistry`, `FileWatcher`, `Indexer`, `IndexQuery`; implements `IWorkspaceServices`) → `AgentCore` (owns `ConversationHistory`, `SessionManager`, bridges `ILLMClient` + `IToolRegistry` + `IWorkspaceServices&`) → frontends receive events via `IFrontend` through `FrontendRegistry`.

**Adding a new file format extractor**: Create `src/extractors/<format>_extractor.h/cpp` implementing `ITextExtractor`. Register it in `Workspace::Workspace()` (`workspace.cpp`) with `extractors_->register_extractor(".ext", ...)`. Add the `.cpp` to `locus_core` in `CMakeLists.txt`. No changes to `src/index/indexer.cpp` needed.

---

## Document Map

| File | When to read |
|---|---|
| [CONTRIBUTING.md](CONTRIBUTING.md) | Code style, build/run/test instructions for humans |
| [tests/integration/README.md](tests/integration/README.md) | Manual LLM-driven integration test suite — tags, harness design, update rules |
| [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md) | Retrieval eval harness — gold queries, recall/MRR/nDCG, baseline + regression check |
| [roadmap.md](roadmap.md) | Roadmap index — milestones M0–M5 with one-table summaries. Real detail lives in `roadmap/`. |
| [roadmap/M0.md](roadmap/M0.md), [M1.md](roadmap/M1.md), [M2.md](roadmap/M2.md) | Completed/in-progress milestones — full task lists, one file per milestone |
| [roadmap/M3/](roadmap/M3/) | M3 Refactoring — one file per stage (S3.A–S3.K) |
| [roadmap/M4/](roadmap/M4/) | M4 Agent Quality — one file per stage (S4.A–S4.V) |
| [roadmap/M5/](roadmap/M5/) | M5 Connected — one file per stage (S5.1–S5.6) |
| [test-workspaces.md](test-workspaces.md) | Three concrete test cases — read when scoping features |
| [vision.md](vision.md) | Understanding the *why* behind any design decision |
| [DIFFERENTIATORS.md](DIFFERENTIATORS.md) | Who this is for, what makes it different |
| [requirements.md](requirements.md) | Full feature list — reference when scoping work |
| [architecture/overview.md](architecture/overview.md) | System component map, data flow, context strategy |
| [architecture/agent-loop.md](architecture/agent-loop.md) | AgentCore turn orchestration — threading, phases, sequence diagrams, M4 hook points |
| [architecture/threading-model.md](architecture/threading-model.md) | Thread inventory, shared-resource protectors, cross-thread invariants, checklist for new threads |
| [architecture/tool-protocol.md](architecture/tool-protocol.md) | ITool interface, approval flow, adding tools |
| [architecture/workspace-index.md](architecture/workspace-index.md) | Index subsystem: schema, update strategy, query API |
| [architecture/tech-stack.md](architecture/tech-stack.md) | Tech stack — decided choices and future frontend options |
| [architecture/web-retrieval.md](architecture/web-retrieval.md) | Web RAG: fetch → index → search pipeline, token budget strategy |
| [architecture/decisions/](architecture/decisions/) | ADRs — *why* behind non-trivial architectural shifts (one file per decision) |

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

**Integration tests (`tests/integration/`, manual, LLM-driven):**
- **Update them whenever tools, extractors, the agent loop, the approval gate, or the slash-command dispatcher change** — schema changes, new tool, retired tool, changed arg shape, new extractor, new approval path all need matching test coverage in the relevant `test_int_*.cpp`. Add a new `TEST_CASE` tagged `[integration][llm][<topic>]`; don't silently skip.
- **Do NOT run them by default.** They need a live LLM, take minutes, and are not part of the per-stage verification protocol above. Only run when explicitly asked ("run the integration tests", "run [search] integration").
- **When running on request, always pass `-console`.** The flag pops a dedicated console window and streams trace-level logs there live. The user wants to SEE the run (tool calls, LLM stream, SQL, FTS), not a silent 5-minute wait ending in a pass/fail summary. Omit `-console` only if the user explicitly asks for silent mode.
- **Consequence of `-console` when you launch via Bash/subprocess:** the exe redirects its stdout/stderr into the new window, so your captured output will look empty. The subprocess call still completes cleanly when tests finish (no pause). For pass/fail, rely on the exit code; for post-mortem detail point the user at `<workspace>/.locus/integration_test.log`.
- **Minimum verified model: Gemma 4 E4B @ 8k context.** Larger / more capable models are fine. Any model without tool-calling support will fail most cases.
- See [tests/integration/README.md](tests/integration/README.md) for the full run matrix, env vars, and harness design notes.

**After every completed stage/task — mandatory bookkeeping:**
1. Update `roadmap.md`: mark completed tasks `[x]`, add ✔ to the stage header
2. Update `CLAUDE.md` "Current Stage" line to reflect the new state
3. **ADRs are rare.** Only write one if the stage crossed a system boundary
   (technology swap, subsystem reshape, ownership/threading/data-format change) **and**
   rejected a reasonable alternative for a non-obvious reason **and** that reasoning
   isn't already captured in the stage doc or an inline comment. Interface
   refactors, file moves, and ergonomic choices do **not** qualify — the stage doc's
   "Implementation notes" section is the right home for those. See
   [architecture/decisions/README.md](architecture/decisions/README.md) for the full
   bar. When in doubt, don't write one — ask first.
4. Do this immediately after verification passes, before moving on

---

## Token Efficiency Notes

When working on this project's own code:
- Read files before editing (read the specific section, not whole file)
- Use Grep for finding specific patterns before reading
- The architecture docs are the source of truth — don't re-derive what's written there
- This CLAUDE.md file should stay concise — it's a quick-scan reference, not a second vision doc
