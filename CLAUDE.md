# Locus -- Claude Working Guide

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
  Instant UI response is a hard requirement -- no input lag, no phantom waits.
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
- Prefer direct, efficient solutions -- no speculative complexity
- When suggesting architecture, lean toward lean and explicit over framework-heavy
- Treat them as a senior dev -- skip basics, go straight to substance

---

## Doc Conventions

Write laconically. Engineers, not book writers. Narrow wording, no padding -- but don't lose useful info.

- Don't restate what per-stage docs (`roadmap/<milestone>/<stage>.md`), `git log`, the Code Map below, or `architecture/` already cover. Link, don't copy.
- After completing a stage, the CLAUDE.md update is a one-line list edit (add the stage code to the done list). The narrative belongs in the per-stage doc, not here.

*Calibration note: the milestone-list format above (vs. per-stage paragraphs) is tuned for steady-state work. If cross-stage interactions become a constant concern -- e.g., a heavy multi-stage refactor period where seeing how recent stages compose matters -- the diary view is genuinely faster and restoring per-stage paragraphs in "Current Stage" may be worth their cost again. Revisit then.*

---

## Current Stage

**M6 -- Connected & Misc -- in progress.** Done: S6.10 Tasks I + J (S6.11 follow-ups -- `ITool::short_description()` virtual swapped in by `ToolRegistry::build_entry` + `SystemPromptAssembly::build` when `lazy_manifest=true`; <60-char overrides on all 18 registered built-ins; Task J skips the prose `## Available Tools` section entirely when `lazy_manifest && (tool_format==OpenAi || Auto)` and renders a one-line pointer at the API tools[] array + describe_tool instead, since the prose section is pure duplication of the API array those formats consume; Qwen/Claude formats still render the full bullet list because they don't consume the API array; S6.10 Tasks A-H still pending), S6.11 (lazy tool manifest -- `WorkspaceConfig::lazy_tool_manifest`, describe_tool meta-tool, summary-mode for both system-prompt `## Available Tools` section and the API tools array), S6.12 (system-prompt profiles -- `WorkspaceConfig::system_prompt_profile` = full / compact / minimal, controls the prose body without touching workspace metadata / LOCUS.md / memory / tools / format addendum), S6.13 (reasoning watchdog + auto-nudge -- per-round trigger on `reasoning_max_seconds` / `reasoning_max_chars` with OR semantics; `reasoning_auto_nudge=true` cancels the in-flight LLM stream and injects a "Stop reasoning, commit now" steering message; 2-nudge cap before turn aborts; `ILocusCore::request_commit_now()` for programmatic / future-button callers). Per-stage detail in [roadmap/M6/](roadmap/M6/). Cross-cutting design rationale in [ADR-0007](architecture/decisions/0007-context-budget-reshape-lazy-manifest-and-profiles.md).

**M5 -- Polish, UX & Performance -- complete.** Done: S5.A, S5.B, S5.C, S5.D, S5.F, S5.G, S5.I, S5.J, S5.K, S5.L, S5.M, S5.N, S5.O, S5.P, S5.Q, S5.R, S5.S, S5.Z (tasks 1-10 + agentic-mode follow-ups from `tests/ui_automation/output/agentic_Tetris/findings.md` and `tests/ui_automation/output/agentic_TestLocalVibe/findings.md`). Per-stage detail in [roadmap/M5/](roadmap/M5/). S5.Z stays the landing pad if new polish-shaped work surfaces post-close.

**M4 -- Agent Quality -- complete.** Done: S4.A, S4.B, S4.D, S4.F, S4.G, S4.I, S4.J, S4.K, S4.L, S4.M, S4.N, S4.P, S4.R (Phase 1), S4.S, S4.T, S4.U, S4.V, S4.W, S4.X, S4.Y. Per-stage detail in [roadmap/M4/](roadmap/M4/). S4.C parked -- see [roadmap/backlog/S4.C-auto-verify.md](roadmap/backlog/S4.C-auto-verify.md).

**M3 -- Refactoring -- complete.** S3.A through S3.L done. Per-stage detail in [roadmap/M3/](roadmap/M3/).

Note: M3 is Refactoring (not Agent Quality). Renumbering trail: original M3 -> M4 (Agent Quality), original M4 -> M6 (Connected & Misc), new M5 (Polish, UX & Performance) inserted. M6 was later renamed from "Connected" to "Connected & Misc" once non-connectivity stages started landing there. Roadmap index: [roadmap.md](roadmap.md).

### Key invariants

Cross-cutting constraints that no single file owns. Easy to break silently by a refactor in a neighbouring file.

- **Byte-stable system prompt** (S4.F). `messages[0]` is byte-identical across a session so LM Studio / llama.cpp prefix-cache survives. Attached files + file-change notes prepend to the user message per turn, never splice into system. `SystemPromptAssembly` enforces immutability at the type level. Canary: per-round `LLM POST: sys_hash=<hex>` log.
- **Single-writer history** (S3.I, S5.J). Only the agent thread writes `LLMContext::history()`. `AgentLoop` returns appendable results; `ToolDispatcher` appends via a callback running on the agent thread. `ConversationOwnerScope` is the RAII fence.
- **Index is never written by the LLM.** Index updates flow only from the file watcher into `Indexer`. No tool mutates `index.db` / `vectors.db`. Adding a tool that touches the index breaks a core principle.
- **Workspace teardown order.** `LocusSession` member declaration order matters: `mcp_` declared after `tools_` so `tools_` (which holds `McpTool` entries) destructs first, before the `McpClient` instances those entries borrow. `Workspace` dtor terminates `ProcessRegistry` before closing the watcher. Adding a subsystem requires checking these dependencies. Sibling rule for the **`indexer().on_activity` / `embedding_worker().on_activity` callbacks** -- they capture the agent by raw pointer, and `Workspace::~` runs `WatcherPump::stop()` which does a final synchronous flush that can fire `on_activity`. Any owner of (workspace + agent) MUST null those callbacks before the agent destructs (see `LocusSession::~LocusSession` and `IntegrationHarness::~IntegrationHarness`); otherwise the flush dereferences a freed agent.
- **Atomic mutating writes** (S5.O). `edit_file` / `write_file` route through `write_atomic` (temp file + rename, copy fallback for cross-volume). Direct `std::ofstream` to the target reintroduces the half-written-file bug on process kill.
- **FTS5 query sanitization on the production path** (S5.N). User-typed search queries hit FTS5's strict grammar -- punctuation like `/`, `.`, `,` raise `fts5: syntax error near X` and return zero rows silently. `IndexQuery::search_text` routes the query through `sanitise_fts_query` before binding (strip set: `"'():*-^!/.,;?+=<>{}[]|&#@~\$%`` plus backtick and backslash). The eval harness at [tests/retrieval_eval/eval_runner.cpp](tests/retrieval_eval/eval_runner.cpp) keeps a matching `sanitise_for_fts` -- the two strip sets must stay in sync. Any new tool that binds a user-supplied string to an FTS5 `MATCH` must either go through `search_text` or replicate the strip.
- **Info-level LLM stream heartbeat** (S5.Z TestLocalVibe follow-up). [src/llm/openai_transport.cpp](src/llm/openai_transport.cpp)'s `WriteCallback` MUST emit an info-level `"LLM stream: chunk #N (+B B, gap G ms, total T B)"` line every 32 chunks OR every 10 s (whichever first). Trace level is wrong -- `--agentic-mute-noise` drops the spdlog flush threshold to info, so trace lines stay in the buffer for tens of minutes during long reasoning rounds. `wait_for_agent_idle` (op_dispatcher.cpp) reads log mtime as its "agent alive" signal; without the heartbeat it false-positive-reports `status:"stuck"` on any reasoning-heavy local model. Don't demote this back to trace, and don't widen the 10s wall-clock fallback past ~15 s -- both regressions reintroduce the TestLocalVibe finding.
- **Per-area trace logging via `log_db()` / `log_fs()`** (S5.Z agentic follow-up). SQL exec/prepare in [src/core/database.cpp](src/core/database.cpp) goes through `locus::log_db()` (header [src/core/log_channels.h](src/core/log_channels.h)); file-watcher / watcher-pump / indexer / index-query / embedding-worker / gitignore / tree-sitter-registry traces go through `locus::log_fs()`. Both child loggers share the default logger's sinks but their levels are independent, so `mute_noise_loggers()` (called from `LocusApp::OnInit` when `--agentic-mute-noise` is on argv) can demote them to info without losing LLM / tool / agent trace output. Any new SQL-binding or watcher-firing trace line must use the matching channel; calling `spdlog::trace(...)` from those areas reintroduces the noise problem the [agentic Tetris findings](tests/ui_automation/output/agentic_Tetris/findings.md) flagged.

---

## Key Decisions Made

| Decision | Choice | Rationale |
|---|---|---|
| Project name | **Locus** | Unique, precise, workspace-centric |
| Workspace config dir | `.locus/` | Lives inside each workspace root |
| Deployment target | **Standalone app** | Independent of any editor, all use cases |
| Build sequence | **CLI prototype -> desktop UI** | Validate hard parts before committing to UI |
| Core language | **C++20** | User's native language, zero runtime overhead |
| Build system | **CMake + vcpkg** | Standard C++ toolchain |
| Compiler | **MSVC 19.50 (VS 2026)** | Generator: `Visual Studio 18 2026` |
| Index database | **SQLite + FTS5**, split into `.locus/index.db` (skeleton) + `.locus/vectors.db` (semantic, optional) | Zero config, native C API. Separate files + separate connections = independent WALs, so background embedding never blocks FTS/symbol writes. vectors.db is a detachable cache -- delete it to reclaim disk or force re-embed. |
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
| PDF extractor | **PDFium (bblanchon prebuilt, BSD-3-Clause)** | Fetched via `FetchContent` -- not in vcpkg mainline. Ships as `pdfium.dll` alongside `locus.exe` |
| DOCX/XLSX reader | **miniz + pugixml** | miniz unzips OOXML package; pugixml walks `word/document.xml` / sheet XML |
| Index embedder | **llama.cpp (GGUF)** | CPU-only in-process inference; WordPiece vocab bundled in the GGUF; replaced ONNX Runtime which had MSVC-static-CRT schema-registration issues |
| Web search API | **Brave Search (default)** | Clean JSON API, free tier, configurable |
| License | **MIT** | Maximum permissive, community-friendly |

---

## Coding Conventions

Established patterns -- follow these for consistency across the codebase.

**Namespace**: All project code lives in `namespace locus`.

**File layout**: `src/name.h` (header) + `src/name.cpp` (implementation). No `include/` directory --
headers and sources live together in `src/`.

**CMake structure**: Core code is a static library (`locus_core`). Both the main executable (`locus`)
and the test executable (`locus_tests`) link against it. New `.cpp` files go into `locus_core`'s
source list in the root `CMakeLists.txt`.

**Class style**: RAII wrappers for external resources (SQLite, efsw). Rule of five -- delete
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
test names, and log strings. Em-dashes (`--`, U+2014) break ctest test-name dispatch on Windows
(no CP437 mapping) and complicate grep/diff. This rule applies to anything written into the
repo, including this file.

**Static CRT**: vcpkg triplet `x64-windows-static` with custom overlay at `cmake/triplets/`
that adds `-D_USE_STD_VECTOR_ALGORITHMS=0` to work around missing SIMD intrinsics in `libcpmt.lib`.
This flag must be set both in the triplet (for vcpkg deps) and in `CMakeLists.txt`
(for project code).

---

## Build & Test Commands

Copy-pasteable commands. Full reference and rationale in [CONTRIBUTING.md](CONTRIBUTING.md).

**Build dirs are per-config, not per-`-DCMAKE_BUILD_TYPE`.** The repo expects
`build/release/` and `build/debug/` as separate configured trees -- using a single `build/`
will fail with "not a CMake build directory (missing CMakeCache.txt)". Both are already
configured in this checkout.

### Configure (only needed once per dir, or after CMakeLists changes)

```
cmake -B build/release -G "Visual Studio 18 2026" \
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets

cmake -B build/debug -G "Visual Studio 18 2026" \
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets
```

### Build

```
# Default target = locus + locus_gui + locus_tests (skips manual harnesses)
cmake --build build/release --config Release
cmake --build build/debug   --config Debug

# Single target
cmake --build build/release --config Release --target locus
cmake --build build/release --config Release --target locus_gui
cmake --build build/release --config Release --target locus_tests
cmake --build build/release --config Release --target locus_integration_tests
cmake --build build/release --config Release --target locus_retrieval_eval
cmake --build build/release --config Release --target locus_ui_tests
```

| Target | Output (Release) |
|---|---|
| `locus` (CLI)         | `build/release/Release/locus.exe` |
| `locus_gui`           | `build/release/Release/locus_gui.exe` |
| `locus_tests`         | `build/release/tests/Release/locus_tests.exe` |
| `locus_integration_tests` | `build/release/tests/integration/Release/locus_integration_tests.exe` |
| `locus_retrieval_eval`    | `build/release/tests/retrieval_eval/Release/locus_retrieval_eval.exe` |
| `locus_ui_tests`          | `build/release/tests/ui_automation/Release/locus_ui_tests.exe` |

`pdfium.dll` is auto-copied next to `locus.exe` and `locus_gui.exe` by a `POST_BUILD`
step on each app target.

### Run

```
# CLI
build/release/Release/locus.exe <workspace_path> [--endpoint URL] [--model NAME] [--context N] [-verbose]

# GUI
build/release/Release/locus_gui.exe [<workspace_path>]
```

Always run against `d:/Projects/AICodeAss/` (WS1) for stage verification.

### Unit tests (Catch2, fast, hermetic)

```
# Whole suite
build/release/tests/Release/locus_tests.exe

# Through ctest (optional wrapper)
(cd build/release && ctest -C Release --output-on-failure)

# Tag filter
build/release/tests/Release/locus_tests.exe "[s4.t]"
build/release/tests/Release/locus_tests.exe "[indexer]"

# Test-name filter (whole-name match, supports wildcards)
build/release/tests/Release/locus_tests.exe "WriteFileTool: *"

# List
build/release/tests/Release/locus_tests.exe --list-tests
build/release/tests/Release/locus_tests.exe --list-tags
```

### Integration tests (live LLM, manual only -- do NOT run by default)

Need LM Studio at `http://127.0.0.1:1234` with a tool-calling model loaded
(min: Gemma 4 E4B @ 8k context).

```
# Run inline -- stdout / stderr come back through the subprocess pipe so
# Claude sees results immediately. Filter llama.cpp boot noise with grep
# when only the Catch2 / harness lines matter.
build/release/tests/integration/Release/locus_integration_tests.exe

# One tag area
build/release/tests/integration/Release/locus_integration_tests.exe "[smoke]"
build/release/tests/integration/Release/locus_integration_tests.exe "[search]"
# (also: [outline] [fs] [shell] [bg] [ask_user] [slash] [file_change_awareness] [undo] [metrics] [max_tokens] [plan] [mcp])
```

For a foreground-console pop-out when running locally for a human to watch,
add `-console` -- the exe redirects stdout/stderr into the new window, so a
subprocess capture comes back empty in that mode. Default is inline.

### Retrieval eval (manual)

```
build/release/tests/retrieval_eval/Release/locus_retrieval_eval.exe \
    --workspace . \
    --queries tests/retrieval_eval/queries.json \
    --out tests/retrieval_eval/results.md
```

### UI automation tests (manual, Windows-only, S5.L)

Scripted UIA harness drives `locus_gui.exe` from a subprocess. Pops real
windows on the desktop session -- run in a secondary session if you don't
want focus stolen. **Manual only -- do NOT run by default.**

```
# All bundled scripts (smoke + settings_tour + chat_round_trip)
build/release/tests/ui_automation/Release/locus_ui_tests.exe --all

# One script
build/release/tests/ui_automation/Release/locus_ui_tests.exe \
    --script tests/ui_automation/scripts/smoke.json
```

Exit codes: 0 pass / 1 fail / 2 usage error / 77 all skipped. Per-script
artifacts (screenshots, run.log, JUnit XML) land in `tests/ui_automation/output/`.
See [tests/ui_automation/README.md](tests/ui_automation/README.md) for step
ops, naming conventions, and the focus-stealing caveat.

See [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md) for baseline
management and the regression-check protocol.

---

## Code Map

Source is in `src/`, tests in `tests/`, architecture docs in `architecture/`.
Core is a static lib (`locus_core`). Both `locus` (exe) and `locus_tests` link it.

| File | What it owns | Key types |
|---|---|---|
| `src/core/locus_session.h/cpp` | Single-ctor/dtor bundle of Workspace + ILLMClient + ToolRegistry + AgentCore (S3.G). Ctor handles wiring order + LLM-config layering (defaults -> `.locus/config.json` -> caller seed -> live `query_model_info`); dtor stops the agent thread first, then unwinds in reverse declaration order. Both CLI (`main.cpp`) and GUI (`LocusApp`) construct one of these per open workspace; future lifecycle subsystems (LSP, MCP) plug in here. | `LocusSession` |
| `src/core/workspace.h/cpp` | Opens a folder, owns main_db (always) + vectors_db (optional) + watcher + indexer + query + watcher_pump. One per folder. `WorkspaceConfig` is the single source of truth for per-workspace knobs (see the header for the full list -- recent additions: `tool_max_runtime_s`, `dump_on_run_command_hang`, `max_rounds_per_message` from the agentic Tetris follow-ups; `run_command_truncate_lines` for the default head+tail line count `run_command` / `read_process_output` keep before sending output to the LLM; **S6.11** `lazy_tool_manifest` toggles describe_tool + summary-mode for the per-turn tool catalog; **S6.12** `system_prompt_profile` selects full / compact / minimal prose body; **S6.13** `reasoning_max_seconds` + `reasoning_max_chars` + `reasoning_auto_nudge` cap a single LLM round's reasoning budget). | `Workspace`, `WorkspaceConfig` |
| `src/core/database.h/cpp` | RAII SQLite wrapper. WAL mode. Two schema kinds: `Main` (files/fts/symbols/headings) and `Vectors` (chunks + `meta` + vec0 chunk_vectors; loads sqlite-vec only here). `chunk_vectors` is created lazily by `ensure_vectors_schema(dim)`, which also performs the dim-mismatch wipe + re-embed migration. Includes legacy-table migration from the pre-S3 split. SQL exec/prepare traces route through `log_db()` (see [log_channels.h](src/core/log_channels.h)) so agentic-mode mute can silence them independently. | `Database`, `DbKind` |
| `src/core/log_channels.h/cpp` | Per-area spdlog child loggers (`db`, `fs`) that share the default logger's sinks but carry independent levels. `mute_noise_loggers()` demotes both to info and drops the global `flush_on` threshold to info so `read_locus_log` ops see fresh content. Called from `LocusApp::OnInit` when `--agentic-mute-noise` is on argv (added automatically by the agentic-server `op_launch`). Also exposes `truncate_for_log(s, cap=1024)` -- shared helper for trace lines that quote tool-call args / tool results so a multi-KB blob doesn't dominate the log (agentic Tetris run 1 hit this with a 3-KB `edits` payload). | `log_db`, `log_fs`, `mute_noise_loggers`, `truncate_for_log` |
| `src/core/file_watcher.h/cpp` | efsw wrapper. `drain()` returns events past a 200 ms debounce window; `drain_all()` (used by `WatcherPump::flush_now`) returns everything pending without waiting. `push_raw` falls back to `lexically_relative` when `fs::relative` throws on a transient Windows `weakly_canonical: Access is denied` (AV / file-lock race) instead of dropping the event. | `FileWatcher`, `FileEvent` |
| `src/index/indexer.h/cpp` | Initial traversal + incremental updates. Owns `IndexerStatements` + `TreeSitterRegistry` + `SymbolExtractorRegistry`; uses `ExtractorRegistry` for text/heading extraction. Writes skeleton to main_db, chunks to vectors_db -- transactions open on both connections in parallel. After S3.D, `index_file` is purely orchestration: read -> extract -> upsert -> symbols -> chunk -> persist. | `Indexer`, `Indexer::Stats` |
| `src/index/index_query.h/cpp` | Read-only queries: FTS5 search, symbol lookup, outlines, dir listing. Semantic search is a two-step read -- vectors_db for top-K chunks, main_db to resolve file_id -> path. S5.N reshaped `search_text` snippet generation: FTS5's `snippet(files_fts, 1, '', '', '...', 24)` over the indexed extracted column replaces the old "slice raw file bytes" path (which dragged HTML / CSS / PDF markup into the LLM context on non-code workspaces). Line-number recovery is now best-effort via `find_match_line` on the raw file for code-like files (raw == extracted) and 0 for extractor-driven formats. Same stage added `sanitise_fts_query` (Key Invariant -- see top of doc) so user queries containing `/`, `.`, `,`, etc. don't raise `fts5: syntax error near X` and silently return zero rows. | `IndexQuery`, `SearchResult`, `SymbolResult`, `OutlineEntry`, `FileEntry` |
| `src/index/tree_sitter_registry.h/cpp` | Owns the `TSParser*` and the `name -> TSLanguage*` map (9 grammars). `Indexer` holds one as a member; designed to be shared with S4.M `ast_search`. Caller must serialise parser use. | `TreeSitterRegistry` |
| `src/index/symbol_extractor.h/cpp` | `ISymbolExtractor` interface returning `vector<ExtractedSymbol>`, plus a `SymbolExtractorRegistry` keyed by language name. The shared two-level walk + `extract_name_from_node` helper live on a private `RuleBasedSymbolExtractor` inside the .cpp. `register_builtin_symbol_extractors()` wires the seven languages into the supplied registry. | `ISymbolExtractor`, `ExtractedSymbol`, `SymbolRule`, `SymbolExtractorRegistry` |
| `src/index/symbol_extractors/<lang>_extractor.cpp` | One file per language (c / cpp / python / js_ts / go / rust / java / csharp + S4.Y ruby / php / bash / swift / kotlin). Each holds only its `SymbolRule` table and a `make_<lang>_symbol_extractor()` factory that forwards to `make_rule_based_symbol_extractor`. No DB or Tree-sitter walking code. C has its own rule set (typedefs + unions, no class/namespace); the S4.Y additions cover their respective grammars. | -- |
| `src/index/prepared_statements.h/cpp` | RAII holder for the 14 indexer prepared statements (10 main.db + 4 vectors.db; the vectors-db ones stay null when semantic search is disabled). ctor prepares everything, dtor finalises. Members are public so `Indexer` accesses them as `stmts_.upsert_file` etc. | `IndexerStatements` |
| `src/index/chunker.h/cpp` | Content chunking: code (Tree-sitter boundaries), document (heading boundaries), sliding window fallback. | `Chunk`, `SymbolSpan`, `chunk_code()`, `chunk_document()`, `chunk_sliding_window()` |
| `src/index/glob_match.h` | Minimal glob matcher (`*`, `**`, `?`) used by the indexer's exclude patterns and the regex-search tool. Header-only. | `glob_match()` |
| `src/index/gitignore.h/cpp` | S4.L `.gitignore` parser + matcher. `parse_gitignore_text` normalises one file's lines into glob patterns scoped to the file's directory; `load_workspace_gitignore` walks the workspace (skipping `{.git, .locus, node_modules, build, dist, out, target}` to avoid pulling thousands of patterns from vendored sub-repos under `build/`); `gitignore_match` is a small recursive matcher with proper `**` backtracking (the simpler `glob_match` doesn't backtrack on inner `*` failures). Negation patterns (`!foo`) are dropped per S4.L spec. | `GitignorePattern`, `parse_gitignore_text`, `load_workspace_gitignore`, `gitignore_match` |
| `src/llm/llm_client.h/cpp` | Public `ILLMClient` interface + types (`ChatMessage`, `ToolCallRequest`, `LLMConfig`, `StreamCallbacks`, `CompletionUsage`, `ToolSchema`, `ModelInfo`, `ToolFormat`). The .cpp holds the concrete `LMStudioClient` (composes `OpenAiTransport` + a per-`ToolFormat` `IStreamDecoder`), the `create_llm_client` factory, and the `make_decoder_for` helper. Post-S3.B: no `estimate_tokens` static -- see `TokenCounter`. | `ILLMClient`, `LMStudioClient`, `ChatMessage`, `LLMConfig`, `StreamCallbacks`, `ToolFormat` |
| `src/llm/openai_transport.h/cpp` | cpr POST `/v1/chat/completions` + SSE feed + one-shot stall retry + HTTP error reporting. Hands each `data:` payload to its callback as a raw string and swallows `[DONE]`. Knows nothing about JSON. Emits an info-level chunk heartbeat every 32 chunks OR every 10 s (Key Invariant -- see "Info-level LLM stream heartbeat" above; closes TestLocalVibe finding #1). Also emits `first chunk after X ms` (info), `inter-chunk gap X ms` (warn at >60s), and an end-of-stream summary. | `OpenAiTransport` |
| `src/llm/sse_parser.h/cpp` | Low-level SSE `data:` line parser. Used by `OpenAiTransport`. | `SseParser` |
| `src/llm/stream_decoder.h` | `IStreamDecoder` interface -- translates one SSE payload into typed events (`on_text` / `on_reasoning` / `on_tool_call_delta` / `on_usage` / `on_finish_reason` / `on_stream_error`) via a stack-allocated `StreamDecoderSink`. Decoders may keep per-stream state (XML formats need it). Three optional hooks: `decode()` per chunk, `finish_stream(sink)` once at end-of-stream to flush held-back partial-tag bytes (S4.N), and `reset()` between streams. `on_finish_reason` and `on_stream_error` (S4.U) carry `length` / `content_filter` / refusal / mid-stream `{"error":...}` events the LMStudioClient turns into user-visible warnings. | `IStreamDecoder`, `StreamDecoderSink` |
| `src/llm/xml_tool_call_extractor.h/cpp` | S4.N -- streaming filter that scans a text channel for embedded XML tool calls (`<tool_call>{json}</tool_call>` Qwen, `<function_calls><invoke...>...</invoke></function_calls>` Claude) and extracts them as `ToolCallDelta` events. Holds back partial-marker suffixes across chunks; truncated bodies surface as text on `finish()`. Shared by Qwen / Claude / Auto decoders below. Qwen body parser also recognises the Hermes / GLM dialect (`<function=NAME><parameter=PNAME>VALUE</parameter>...</function>`, plus `name="..."` attribute form) so models that wrap a non-JSON body inside `<tool_call>` still dispatch instead of dropping with a JSON-parse warn. `next_index() / set_next_index()` exists so a decoder running two extractor instances (one per channel) can share a single counter across content + reasoning, keeping ToolCallDelta `index` slots + synth `call_xml_<n>` ids unique. | `XmlToolCallExtractor`, `XmlMarker` |
| `src/llm/stream_decoders/openai_decoder.h/cpp` | Default decoder for OpenAI-compatible chunks (`choices[0].delta.{content,reasoning_content,tool_calls}` + top-level `usage`). Stateless across calls. | `OpenAiDecoder` |
| `src/llm/stream_decoders/qwen_xml_decoder.h/cpp` | S4.N -- composes `OpenAiDecoder` (chunk shape) with two `XmlToolCallExtractor{Qwen}` instances (one per channel: content + reasoning -- a reasoning model that emits its `<tool_call>` inside `<think>`/`reasoning_content` would otherwise leave the call dead in the thinking bubble). Native JSON `tool_calls` deltas pass through; embedded `<tool_call>` markers are extracted with a synthetic `call_xml_<n>` id and JSON-parsed body. Shared counter on `Impl` syncs the two extractors so ids / indices stay unique across channels. | `QwenXmlDecoder` |
| `src/llm/stream_decoders/claude_xml_decoder.h/cpp` | S4.N -- same shape as Qwen but for `<function_calls><invoke...>` blocks; multiple invokes per block emit one `ToolCallDelta` each. Parameter values are run through a JSON-parse-with-string-fallback so numbers / booleans / arrays land as their natural types. Decodes the five common XML entities. Same dual-extractor + shared-counter layout as Qwen (covers reasoning-channel tool calls). | `ClaudeXmlDecoder` |
| `src/llm/stream_decoders/auto_decoder.h/cpp` | S4.N default. Watches both Qwen and Claude markers simultaneously across content **and** reasoning channels (two `XmlToolCallExtractor` instances + shared counter); the OpenAI native path remains untouched, so this is the right pick when the user hasn't pinned a `tool_format` for their model. | `AutoToolFormatDecoder` |
| `src/llm/llm_router.h/cpp` | S4.Q hook. Owns a strong + optional weak `ILLMClient`; `select(Role)` returns the right one (today: weak falls back to strong). Not yet wired into `LocusSession`. | `LlmRouter` |
| `src/llm/model_presets.h/cpp` | S4.V Task 6 static known-good defaults per local-LLM backend + model family (LM Studio + Gemma / Qwen / Llama 3.x / non-tool-trained, Ollama generic, llama-server generic). `ModelPreset` carries `base_url` / `temperature` / `max_tokens` / `tool_format`; the model id is intentionally not part of presets because every install has different specific files. `builtin_presets()` returns the table in dropdown order; `find_preset(name)` is the by-name lookup. Consumed by the Settings -> LLM tab's "Load preset" row. | `ModelPreset`, `builtin_presets`, `find_preset` |
| `src/llm/token_counter.h/cpp` | Heuristic token estimator (~4 chars = 1 token, +4 framing per message). Lifted off `ILLMClient` in S3.B so callers (`AgentCore`, `AgentLoop`, `ConversationHistory`, `SystemPromptBuilder`, `CompactionDialog`, tests) don't drag the LLM client interface in just to count tokens. S4.F will introduce real BPE tokenisation behind this same surface. | `TokenCounter` |
| `src/core/workspace_services.h` | `IWorkspaceServices` interface -- tool-facing surface over a workspace (no .cpp -- pure abstract). `Workspace` implements it; tests use `tests/support/fake_workspace_services.h`. | `IWorkspaceServices` |
| `src/core/watcher_pump.h/cpp` | Background thread that drains FileWatcher, batches by quiet-period (1.5s) + hard-cap (20s), calls `Indexer::process_events`. Owned by `Workspace` -- replaces the pump that used to live duplicated in main.cpp / locus_app.cpp / locus_frame.cpp. `flush_now()` is the synchronous-flush escape hatch -- it calls `FileWatcher::drain_all` so the 200 ms debounce is bypassed (the caller already decided "drain now"), then routes the batch through the indexer on the calling thread. | `WatcherPump` |
| `src/core/workspace_lock.h/cpp` | RAII exclusive file lock on `.locus/locus.lock`. Acquired first in `Workspace` ctor, released last in dtor. Refuses to open when the same workspace -- or any ancestor workspace -- is already held by another Locus process. OS releases the lock on crash. Replaces the process-global `wxSingleInstanceChecker` in the GUI so two different workspaces can run side by side. | `WorkspaceLock` |
| `src/core/memory_store.h/cpp` | S4.R workspace-scoped memory bank. Owns `.locus/memory/<id>.md` (verbatim entries with YAML frontmatter) + the `memory_fts` FTS5 table in main_db + the `memory_vectors` vec0 table in vectors_db (with a `memory_keys(rowid, id)` mapping so vec0's integer-keyed rows can reference text ids). Add / update / soft_delete (`.deleted/` retention) / hard_delete (skips retention, used by `/forget --hard` and the future Settings UI "delete permanently" button) / restore_deleted / purge_deleted (30-day retention) / gc (max-entries cap). Hybrid `search` blends BM25 + cosine + soft 21-day recency factor + optional reranker; raw queries are phrase-wrapped per whitespace token before binding to FTS5 (`escape_fts5_query` helper) so hyphens / colons / quotes etc. in natural-language input don't get parsed as FTS5 operators -- without this, a query like `S4R-INT-tool-search-6882a8` parses as `S4R NOT INT NOT tool NOT search NOT 6882a8` and matches nothing. `format_for_system_prompt()` composes the "## Memory Bank" slot for the system-prompt builder -- pinned first then LRU within `memory.in_context_budget_tokens`. Owned by `Workspace`, exposed via `IWorkspaceServices::memory()`. Single-writer / multi-reader (per-store mutex); reconciles FTS5 + vector rows against on-disk `.md` on construction so a deleted vectors.db or model swap re-embeds cleanly. | `MemoryStore`, `MemoryStore::Entry`, `MemoryStore::SearchHit` |
| `src/core/memory_filter.h/cpp` | S5.K pure helpers consumed by `MemoryBankPanel` -- not the source of truth, just policy on top of `MemoryStore::list_all`. `apply_filter(entries, MemoryFilter)` applies query / tag-intersection / source / pinned-only / created-at range. `sort_entries(MemorySort)` covers pinned-first variants + content / tags / source / created / last_used asc/desc. `collect_unique_tags` populates the filter dropdown. `list_deleted_stubs(store)` walks `<memory_dir>/.deleted/*.md` returning lightweight `{id, content_preview, tags, deleted_at_proxy}` rows for the "Show deleted" view -- the panel still calls `MemoryStore::restore_deleted` / `hard_delete` to mutate. All functions pure / wx-free so they unit-test without a real UI. | `MemoryFilter`, `MemorySort`, `apply_filter`, `sort_entries`, `collect_unique_tags`, `DeletedEntryStub`, `list_deleted_stubs` |
| `src/tools/tool.h` | Tool system interfaces (no .cpp -- pure abstract + structs). | `ITool`, `IToolRegistry`, `ToolParam`, `ToolResult`, `ToolCall` |
| `src/tools/tool_registry.h/cpp` | Concrete registry. Schema JSON builder. ToolCall parser. S6.11 added a `bool lazy` param to `build_schema_json(ws, mode, lazy)` that, when true, replaces every tool's `parameters` schema with `{type:"object", properties:{}, additionalProperties:true}` -- except `describe_tool` itself which keeps its real schema (the meta-tool is the entry point for fetching the others). S6.10 Task I extended the `lazy=true` path so the description slot also swaps from `description()` / `description_for(ws)` to the curated `short_description()` one-liner for every tool except `describe_tool` (which keeps its full description so the model understands how to call it). | `ToolRegistry` |
| `src/tools/tools.h/cpp` | Aggregator header (re-exports every tool) + `register_builtin_tools()` factory. Individual tools live in the family headers below. | `register_builtin_tools` |
| `src/tools/shared.h/cpp` | `resolve_path` / `make_relative` / `error_result` helpers shared by tool implementations. S5.O added `is_inside(candidate, root)` -- component-aware workspace containment (case-insensitive on Windows, exact elsewhere; lexically_normal'd first so trailing slashes / `.` segments don't shift component count); `resolve_path` calls it after canonical / weakly_canonical resolution so a sibling like `<workspace>-evil` no longer passes the byte-prefix check. S5.O also lifted `write_atomic(target, content)` here out of `file_tools.cpp`'s unnamed namespace -- temp file + rename, with a copy-file fallback on cross-volume rename failure. Namespace `locus::tools`. | `is_inside`, `write_atomic`, `resolve_path` |
| `src/tools/command_path_scanner.h/cpp` | S4.V Task 5 pure scanner. `scan_outside_workspace_paths(cmd, root)` tokenises on whitespace (single/double-quoted runs become one token, quotes stripped) and flags absolute Windows / UNC / Unix paths, parent-traversal segments, and env-var or `~` expansions whose canonical form lies outside `root`. `ToolDispatcher` runs it for `run_command` / `run_command_bg`; non-empty results downgrade `auto_approve` -> `ask` and flow as `safety_warnings` through `IFrontend::on_tool_call_pending` -> approval panel banner. Doesn't apply to file tools (they already refuse outside-workspace paths via `resolve_path`). | `scan_outside_workspace_paths` |
| `src/tools/permission_presets.h/cpp` | S5.S named permission preset matrix. `PermissionPreset` enum (`read_only` / `ask_before_edits` / `allow_edits` / `allow_all` / `custom`); `builtin_tool_category(name)` maps every built-in to one of {read, edit, delete, memory, process, shell, interactive, plan, mcp, other} so new tools slot in by category instead of touching the matrix; `policy_for_category(preset, cat, default)` returns the cell value (read tools always auto; ask_user always ask; plan tools use the tool default; mcp stays `ask` in all four named presets); `preset_signature(preset, registry)` walks the registry and produces the full per-tool override map (including the `mcp:*` wildcard) the panel writes into `tool_approval_policies`; `detect_preset(overrides, registry)` is the reverse function -- compares the effective per-tool map against each named signature and returns the first exact match or `custom`. Per-server `mcp:<name>:*` entries that diverge from the wildcard force Custom. Custom is a sentinel only -- `preset_signature(custom, ...)` returns an empty map. | `PermissionPreset`, `builtin_tool_category`, `policy_for_category`, `preset_signature`, `detect_preset` |
| `src/tools/file_tools.h/cpp` | File CRUD + exact-string-diff edit tool (`EditFileTool` takes an `edits[]` array -- single edit = array of one, batch = N items applied atomically). `WriteFileTool` subsumes the old `CreateFileTool` via an `overwrite` flag (default `false` refuses to replace existing files). | `ReadFileTool`, `WriteFileTool`, `EditFileTool`, `DeleteFileTool` |
| `src/tools/search_tools.h/cpp` | Unified `SearchTool` (mode: text/regex/symbols/semantic/hybrid) -- the only search tool registered in the built-in manifest. The per-mode `Search*Tool` classes remain as internal implementations the dispatcher delegates to. S5.N lowered the default `max_results` (text 20 -> 8, semantic/hybrid 10 -> 5) and rewrote the tool description to name PDF / DOCX / XLSX as first-class searchable formats so LLMs don't claim inability on binary docs. | `SearchTool`, `SearchTextTool`, `SearchRegexTool`, `SearchSymbolsTool`, `SearchSemanticTool`, `SearchHybridTool` |
| `src/tools/index_tools.h/cpp` | Listing + outline tools. `ListDirectoryTool` (S4.W) enumerates the real filesystem via `std::filesystem::directory_iterator` and uses `IndexQuery::list_directory` only for annotation -- one hashmap lookup per filesystem entry merges in indexed `size_bytes` / `language` / `is_binary`. Per-entry annotation: `[<lang>]` for indexed text, `[binary]` for binary-extension files or indexer-flagged binaries, `[oversized]` for files past `WorkspaceConfig::max_file_size_kb`, `[unindexed]` for everything else (model is expected to try `read_file` if it suspects text). Workspace `exclude_patterns` are still enforced at the tool layer (`.git/**`, `node_modules/**`, `build/**`, `.locus/**`, etc.) so excluded dirs never surface; directory entries are checked via a `rel + "/x"` probe so patterns like `.git/**` match the directory itself, not just contents. Output is capped at `max_entries` (default 200, override via new tool param) with a `[N more entries truncated; refine path or use search]` footer. `IndexQuery::list_directory` contract unchanged (still indexed-only) -- the tool is the policy layer. | `ListDirectoryTool`, `GetFileOutlineTool` |
| `src/tools/process_tools.h/cpp` | Synchronous shell execution + S4.I background-process tools (`run_command_bg`, `read_process_output`, `stop_process`, `list_processes`). The bg tools mark `available()=false` when the workspace has no `ProcessRegistry`. Agentic Tetris follow-up dropped `timeout_ms` from `RunCommandTool::params()` -- the LLM kept picking conservative values (e.g. 120 s) that killed legitimate cold-cache cmake builds. The .cpp's `call.args.value("timeout_ms", 1800000)` stays (legacy callers + tests pass it explicitly); only the schema-advertised slot is gone. Default 30 min applies; the Stop button covers user-side interrupt. Same follow-up wired the `output_filter_mode` / `_pattern` / `_lines` / `_context` flat params into both `RunCommandTool` and `ReadProcessOutputTool` -- pre-LLM head/tail/regex/substring trim via [process_output_filter](src/tools/process_output_filter.h) so chatty build logs don't pollute context; default is `head_tail` with `WorkspaceConfig::run_command_truncate_lines` (50) per side. Same change bumped the raw-output backstop on the sync path from 8 KB to 1 MB (the old 8 KB cap was amputating real build logs before the filter could see them) and trace-logs the full pre-filter body so `.locus/locus.log` is the user-side recovery path. | `RunCommandTool`, `RunCommandBgTool`, `ReadProcessOutputTool`, `StopProcessTool`, `ListProcessesTool` |
| `src/tools/process_output_filter.h/cpp` | Shared output-trimming helper for `run_command` / `read_process_output`. `OutputFilterSpec` carries `mode` (`""` / `head_tail` / `head` / `tail` / `regex` / `substring`), `pattern`, `lines`, `context`, `case_sensitive`. `apply_output_filter(text, spec, default_lines)` returns the filtered body with a `[... K lines elided; full output in .locus/locus.log (trace level) ...]` marker so the LLM (and the user) always know content was trimmed; the head_tail / head / tail short-circuits return the original `text` when nothing needs eliding so trailing-newline semantics survive round-trip. `parse_output_filter_args(args, out, err)` reads the four flat params off a `ToolCall::args` JSON. Pure CPU, no FS / workspace deps -- testable in isolation (`tests/test_process_output_filter.cpp`). | `OutputFilterSpec`, `apply_output_filter`, `parse_output_filter_args` |
| `src/tools/process_registry.h/cpp` | Per-workspace registry of long-running shell processes (S4.I). Each child runs under a Win32 Job Object (`KILL_ON_JOB_CLOSE`) so grandchildren spawned via `cmd /c` are killed with the parent; one reader thread per process drains stdout+stderr into a ring buffer capped at `WorkspaceConfig::process_output_buffer_kb`. The registry tracks per-pid last-delivered offset so `read_output(since_offset?)` can default to "everything new since last read". `Workspace` owns the registry and the dtor terminates every still-running child before the workspace closes. S5.B added a `ProcessSinkBroker` owned by the registry -- `append_locked` and the reader-loop exit hook fan out chunks + lifecycle events through it without disturbing the ring-buffer semantics; the sink fires BEFORE the cap so the panel always sees every byte. S5.Z task 4 replaced the bg-spawn's NUL stdin with a real `CreatePipe`; the parent-side write handle (`stdin_write_`) is non-inheritable + mutex-guarded so `write_stdin(std::string_view)` can forward keystrokes from the terminal panel. `ProcessRegistry::write_stdin(id, data)` is the per-id facade. | `ProcessRegistry`, `BackgroundProcess` |
| `src/tools/process_sink.h` | S5.B header-only `IProcessSink` interface + thread-safe `ProcessSinkBroker`. Implemented by `TerminalPanelState` (rendering sink, S5.R) and `LocusFrame::TabProcessObserver` (lifecycle-only observer for auto-show + busy-badge). Called from worker threads (one per `BackgroundProcess`, plus the sync `RunCommandTool` reader). Panels expected to take a short mutex + queue chunks, never to call wx widgets directly. Six methods: `on_bg_{started,chunk,exited}` for `run_command_bg` and `on_sync_{started,chunk,exited}` for `run_command`. S5.R replaced `set_sink(IProcessSink*)` with multi-subscriber `add_sink` / `remove_sink` -- per-tab brokers carry 1-2 subscribers each; the empty-subscriber state stays cheap. Subscribers must NOT call add/remove from inside an emit callback (would deadlock on the broker's mutex). | `IProcessSink`, `ProcessSinkBroker` |
| `src/tools/interactive_tools.h/cpp` | Tools that go through the approval gate to solicit user input. | `AskUserTool` |
| `src/mcp/mcp_types.h` | S4.G -- header-only structs for MCP server config + advertised tool definition (no .cpp). | `McpServerConfig`, `McpToolDefinition` |
| `src/mcp/mcp_config.h/cpp` | S4.G -- loads `.locus/mcp.json` (workspace) and the global `%APPDATA%/Locus/mcp.json`, merges them (workspace overrides global by server name). Format mirrors Claude Desktop / Cursor: `{"mcpServers":{"<name>":{"command","args","env","enabled"}}}`. Missing files yield empty list; parse errors log a warning and skip the offending file. | `McpConfigLoader` |
| `src/mcp/json_rpc.h/cpp` | S4.G -- ~100-LOC JSON-RPC 2.0 helpers: `make_request` / `make_notification` / `make_response` / `make_error` plus a tolerant `parse_message` that returns a typed `Message{kind, id?, method, params, result, error}` with explicit `parse_error` / `invalid` kinds for malformed input rather than throwing. | `Message`, `Kind`, `make_*`, `parse_message` |
| `src/mcp/stdio_transport.h/cpp` | S4.G -- Win32 child-process spawn with bidirectional pipes, merged-env block (parent env + per-server overrides), Job Object (`KILL_ON_JOB_CLOSE`) so the child dies if the parent crashes, line-oriented reader thread that splits stdout on `\n` and fires `on_message(line)`, mutex-serialised `send_line`, and `CancelIoEx`-based shutdown. Stderr is inherited so server diagnostics surface in our log. | `StdioTransport` |
| `src/mcp/mcp_client.h/cpp` | S4.G -- one MCP server connection. Owns the transport, performs the `initialize` handshake (sends `notifications/initialized` after), runs `tools/list` + `tools/call` synchronously via promise/future request map keyed by JSON-RPC id, and tracks status (`not_started -> initializing -> ready -> failed/crashed/stopped`). Pipe close after `ready` flips status to `crashed` and fails every outstanding request so callers don't hang. Optional `on_crash` callback for the manager. | `McpClient`, `McpClient::Status` |
| `src/mcp/mcp_tool.h/cpp` | S4.G -- `ITool` adapter for one MCP-advertised tool. `name()` returns the namespaced `mcp:<server>:<tool>` form; `parameters_schema()` forwards the server's `inputSchema` verbatim (richer than the flat `ToolParam` list -- nested objects, enums, etc. survive); `params()` is a best-effort flat projection for callers that bypass the schema hook; `execute()` round-trips `tools/call` and flattens MCP's `result.content[]` array into a single string for the LLM, mapping `isError=true` to `ToolResult.success=false`; `available()` returns false unless the client status is `ready`. Default approval policy `ask`. | `McpTool` |
| `src/mcp/mcp_manager.h/cpp` | S4.G -- per-workspace MCP pool. `start_all()` reads merged `mcp.json`, spawns each enabled server, blocks on the `initialize` handshake (default 10 s), drains `tools/list`, and registers each tool into the supplied `IToolRegistry` as `mcp:<server>:<tool>`. Failed servers stay in the manager (visible to the future settings UI) but contribute no tools. `restart(name)` hot-reloads a single server. `status_snapshot()` returns the full `[ServerStatus{name, command, status, last_error, tool_names}]` for the GUI. Status listener fires on the McpClient reader thread. Lifetime: declared between `llm_` and `tools_` in `LocusSession` so `tools_` (which holds the McpTool entries) is destroyed before `mcp_` (which holds the McpClient instances those tools borrow). | `McpManager`, `McpManager::ServerStatus` |
| `src/tools/plan_tools.h/cpp` | S4.D Plan-mode tools. `propose_plan` is visible only in plan mode (`visible_in_mode(ToolMode::plan)`); validates `steps[].description`, normalises optional `tools_needed`, returns `{"ok":true,...}` JSON. `mark_step_done` is visible only in execute mode; validates `step` (1-based int) + `status` enum (`done`/`failed`). Both auto-approve -- the user approves the *plan* itself via the chat panel, not per-tool. AgentCore observes the result JSON to update its in-memory `Plan` state. | `ProposePlanTool`, `MarkStepDoneTool` |
| `src/tools/memory_tools.h/cpp` | S4.R memory-bank tools. `add_memory` (approval `ask`) commits a verbatim entry through `MemoryStore::add`; `search_memory` (auto-approve) runs hybrid retrieval and renders hits with verbatim content + tag preview, enforcing a per-call response-token cap so a chatty memory bank can't poison the next round. Both gate themselves with `available(ws) = ws.memory() != nullptr` so a memory-disabled workspace drops them from the per-turn manifest. Tag input accepts either a JSON array or a comma-separated string for tolerance to model output shape. | `AddMemoryTool`, `SearchMemoryTool` |
| `src/tools/filter_output_tool.h/cpp` | S5.Z task 8 regex/substring filter over caller-supplied text. Pure-CPU + no FS access; `approval_policy = auto_approve` and `builtin_tool_category = "read"`. Modes: `regex` (ECMAScript, default) or `substring`; both case-insensitive by default. `before` / `after` add up to 10 lines of context per match (grep -B / -A); `max_matches` (default 50) caps the response; `invert` flips the match. Output uses grep-style `<line>:<text>` for matches and `<line>-<text>` for context, with `--` separators between non-contiguous blocks. CRLF / lone-CR are collapsed before line splitting so a Windows build log doesn't smear into one line. Summary line `X of Y lines matched (capped at N)` always leads the response so the LLM has a count even when the body is trimmed. **Companion**: [process_output_filter](src/tools/process_output_filter.h) is the inline form -- `run_command` and `read_process_output` apply the same shape before the body enters context, so the agent rarely needs to chain a separate `filter_output` call. | `FilterOutputTool` |
| `src/tools/describe_tool.h/cpp` | S6.11 lazy-manifest meta-tool. Always REGISTERED in the registry; `available(ws)` returns `WorkspaceConfig::lazy_tool_manifest`, so the LLM-facing manifest only includes the tool when lazy mode is on. `execute()` returns the full OpenAI-format JSON schema entry for the named tool as content (same shape `ToolRegistry::build_entry` produces with `lazy=false`); unknown name -> `success=false` with a closest-match suggestion (small Levenshtein helper kept private). Approval policy `auto_approve`. Tools panel and slash commands can still inspect via the registry directly even when lazy mode is off. | `DescribeTool` |
| `src/agent/agent_mode.h` | S4.D types. `AgentMode` enum (`chat` / `plan` / `execute`) with string round-trip; `Plan` and `PlanStep` structs (`PlanStep::Status` = pending/in_progress/done/failed); `plan_to_json` helper. Header-only -- the actual mode + plan state lives on `AgentCore`. | `AgentMode`, `Plan`, `PlanStep` |
| `src/index/embedder.h/cpp` | llama.cpp session wrapper. Loads a GGUF model, tokenises via the embedded vocab, runs inference, returns an L2-normalised embedding vector. Dimension is read from the GGUF (`llama_model_n_embd`) and exposed via `dimensions()`; n_ctx defaults to 1024 (clamped to `n_ctx_train`). Workspace owns it as a `shared_ptr` so a process-wide `Workspace::set_embedder_provider` hook (used by the test suite) can hand the same instance to many Workspaces -- single-Workspace production is unaffected. | `Embedder` |
| `src/index/reranker.h/cpp` | llama.cpp wrapper for cross-encoder GGUFs (e.g. `bge-reranker-v2-m3`) with `LLAMA_POOLING_TYPE_RANK`. `score(query, passage)` returns a single relevance logit; `score_batch()` is a convenience over many passages. Used by `search_semantic` / `search_hybrid` when `WorkspaceConfig::reranker_enabled` is true. | `Reranker` |
| `src/index/embedding_worker.h/cpp` | Background thread: consumes chunk queue, calls Embedder, writes embeddings to vectors_db (its own connection -- no lock contention with the indexer writing to main_db). S5.G added `on_activity` (separate from `on_progress` -- which still feeds the file-tree panel chip): fires synchronously from `enqueue()` with `"Embedding queue: +N chunk(s) (done/total)"` and from the worker thread throttled to every 100 chunks OR every 5 seconds (`set_activity_throttle` knobs), plus once at queue drain. Wired in `LocusSession` to `AgentCore::emit_index_event` so a re-index batch surfaces in the Activity panel under the existing `index_event` kind. | `EmbeddingWorker` |
| `src/extractors/text_extractor.h` | Base interface for file format extractors (no .cpp -- pure abstract + structs). | `ITextExtractor`, `ExtractionResult`, `ExtractedHeading` |
| `src/extractors/extractor_registry.h/cpp` | Maps file extension -> `ITextExtractor`. Owned by `Workspace`. | `ExtractorRegistry` |
| `src/extractors/markdown_extractor.h/cpp` | Extracts text + `#` headings from `.md` files. | `MarkdownExtractor` |
| `src/extractors/html_extractor.h/cpp` | Extracts text + `<h1>`-`<h6>` headings from `.html` files. Strips `<script>`/`<style>`, decodes entities. | `HtmlExtractor` |
| `src/extractors/pdf_extractor.h/cpp` | Extracts text from `.pdf` files via PDFium. One pseudo-heading per page. Flags encrypted PDFs. | `PdfiumExtractor` |
| `src/extractors/docx_extractor.h/cpp` | Extracts text + `HeadingN` headings from `.docx` (unzip + parse `word/document.xml`). | `DocxExtractor` |
| `src/extractors/xlsx_extractor.h/cpp` | Extracts text from `.xlsx` sheets (resolves shared strings, emits one heading per sheet). | `XlsxExtractor` |
| `src/extractors/zip_reader.h/cpp` | miniz helper: read a single named entry from a .zip (shared by docx + xlsx). | `read_zip_entry()` |
| `src/core/frontend.h` | Frontend/core interfaces (no .cpp -- pure abstract + enums). S5.G added `IFrontend::on_history_message_added(int id, MessageRole, bool deletable)` + `on_history_message_deleted(int id)` so per-message-delete UIs can map dom bubbles back to `ConversationHistory` ids, plus `ILocusCore::delete_message(int id)` for the chat-side dispatch path. Agentic Tetris follow-up added `IFrontend::on_round_progress(round, max_rounds)` -- broadcast at the top of every AgentLoop round so frontends can render an in-flight "round N/M" footer chip (`max_rounds=0` means the cap is disabled). | `IFrontend`, `ILocusCore`, `ToolDecision`, `CompactionStrategy` |
| `src/core/frontend_registry.h` | Thread-safe frontend registry with exception-isolated fan-out. Header-only. | `FrontendRegistry` |
| `src/agent/conversation.h/cpp` | Conversation history with JSON serialization and compaction. Owned by `LLMContext` since S5.J; `AgentCore` is no longer a direct writer. S5.G: each `ChatMessage` gets a stable `history_id` assigned by `add()` (in-memory only, NOT part of `to_json` -- the LLM wire stays clean). `from_json` re-walks and re-assigns 1..N so save -> close -> open survives even when an earlier session had deletes that left gaps. New `delete_by_id(int id)` removes the matching message and refuses the leading system entry (S4.F invariant). | `ConversationHistory` |
| `src/agent/system_prompt.h/cpp` | Thin forwarder over `SystemPromptAssembly` (S5.J). `SystemPromptBuilder::build()` keeps the same signature for back-compat with existing tests + callers that just need the raw string. The `WorkspaceMetadata` struct still lives here. | `SystemPromptBuilder`, `WorkspaceMetadata` |
| `src/agent/system_prompt_assembly.h/cpp` | S5.J immutable system-prompt type. The only way to construct one is the static `build()` factory; every reader is `const`, no setter exists. Turns the S4.F byte-stable invariant from discipline ("don't recompute `base_system_prompt_`") into a type-level guarantee. Exposes per-section token counts (base / metadata / locus_md / memory / manifest / format addendum); `total_tokens()` is exact-by-construction (sum of per-section counts), so future S5.G stacked-bar UI doesn't need a separate re-estimate. `hash()` is `std::hash` over `full_text()`, used as the S4.F debug canary. S6.11 added `lazy_manifest` + `ws_for_filter` build params: when lazy is on, the `## Available Tools` section degrades to per-tool one-liners (no per-param breakdown) and the section header tells the model how to fetch real schemas via `describe_tool('<name>')`; the `ws_for_filter` arg lets the prompt-side tools loop honour `ITool::available(ws)` so describe_tool is hidden from the prompt when lazy mode is off (mirrors the API tools array filter). S6.12 added `SystemPromptProfile { Full, Compact, Minimal }` + `profile` build param; `prose_for(profile)` picks one of three statically-rendered bodies for the Rules / Editing / Shell / MSVC sections. Workspace metadata / LOCUS.md / memory bank / tools section / format addendum are identical across profiles. Hash differs per profile (deliberate KV-cache key change when the user toggles). S6.10 Task I extended the `lazy_manifest=true` path so each per-tool bullet uses `short_description()` instead of `description()`. S6.10 Task J: when `lazy_manifest && (tool_format == OpenAi || Auto)`, the entire `## Available Tools` section is replaced by a single-line `## Tools` pointer at the API tools[] array + describe_tool -- the prose section is pure duplication of the API array those formats consume. Qwen / Claude formats still render the full bullet list (those formats don't consume the API array, so the prose section IS the listing). | `SystemPromptAssembly`, `SystemPromptProfile`, `to_string`, `profile_from_string` |
| `src/agent/llm_context.h/cpp` | S5.J -- single owner for the conversation's full LLM-facing state: history, immutable `SystemPromptAssembly`, `ContextBudget`, `FileChangeTracker`, optional `CheckpointStore`, attached-context slot, session/turn ids, plus references to the workspace `IWorkspaceServices` + the shared `SessionManager` + the `FrontendRegistry`. `AgentCore` composes one as `ctx_` and operates on it instead of orchestrating the parts directly. S5.G added `add_message` broadcast of `IFrontend::on_history_message_added(id, role, deletable)` (deletable=true only for user messages and assistant messages with no `tool_calls`), `delete_message(int id) -> bool` (zeros cached server token totals + broadcasts `on_history_message_deleted` on success), and re-announce of every loaded message in `load_session` so a frontend attached after load can rebuild its dom -> history map. `ConversationOwnerScope` (S3.I) gates writes to the agent thread by wrapping `ctx_->history()`; the S5.G per-message-delete request is queued onto the agent thread in `AgentCore` so the scope still holds during the actual delete. | `LLMContext` |
| `src/agent/session_manager.h/cpp` | Save/load/list conversation sessions to `.locus/sessions/`. | `SessionManager`, `SessionInfo` |
| `src/agent/checkpoint_store.h/cpp` | Pre-mutation file snapshots for turn-level undo (S4.B). On-disk layout `.locus/checkpoints/<session_id>/<turn_id>/{checkpoint.json, files/<rel_path>}`. Idempotent within a turn, GC keeps last N turns, oversized files (>1 MB) get a manifest entry but no body. | `CheckpointStore`, `CheckpointEntry`, `TurnInfo`, `RestoreResult` |
| `src/agent/activity_log.h/cpp` | Thread-safe activity ring buffer: assigns monotonic ids, broadcasts each event via `FrontendRegistry`, and serves `get_since()` for late-joining frontends. Extracted from `AgentCore` in S3.A. | `ActivityLog` |
| `src/agent/context_budget.h/cpp` | Token accounting + overflow policy. Tracks last server-reported usage and previous-turn total for delta reporting; fires `on_compaction_needed` at the 80% soft threshold and 100% hard limit. Extracted from `AgentCore` in S3.A. | `ContextBudget` |
| `src/agent/metrics.h/cpp` | Thread-safe agent telemetry (S4.S). Per-turn samples, per-tool histogram, retrieval hit rate. `AgentLoop` records `(usage, stream_ms)` per LLM step; `ToolDispatcher` records `(name, ok, ms, content_size)` per call and parses retrieval result counts via `parse_search_result_count`. `to_json()` rides as `"metrics"` in saved sessions; `to_csv()` powers the GUI exporter and the `/export_metrics` slash. | `MetricsAggregator`, `TurnSample`, `ToolStat`, `RetrievalStat` |
| `src/agent/tool_dispatcher.h/cpp` | Tool approval gate + execute + result injection. Resolves per-workspace approval policy, waits on the decision condvar, and writes the tool-result message through an `AppendFn` so `AgentCore` stays the sole writer of `ConversationHistory`. S4.B: `set_turn_context()` arms a per-turn `CheckpointStore` so `edit_file`/`write_file`/`delete_file` snapshot before execute. S5.O: `submit_decision` is keyed by `call_id` -- `pending_decisions_[call_id]` plus an `awaiting_dispatches_` set so a submission for a non-waiting call is dropped with a warn-log instead of waking the wrong dispatch later. Agentic Tetris follow-up wrapped the `tool->execute()` call in `try/catch (std::exception)` (and `...`) so an unhandled exception from a tool surfaces as `ToolResult{success=false, content="[tool 'X' failed with an internal error: ...]"}` instead of unwinding through the agent thread and crashing the GUI process (run-1 #8 repro). Same follow-up routes the per-call args trace through `truncate_for_log` so a multi-KB payload doesn't dominate `.locus/locus.log`. Extracted from `AgentCore` in S3.A. | `ToolDispatcher` |
| `src/agent/agent_loop.h/cpp` | One LLM round trip: builds the tool schema, streams tokens/reasoning/tool-call fragments, emits the `llm_response` activity event, returns a parsed `AgentStepResult` for the orchestrator. No knowledge of tool execution. Extracted from `AgentCore` in S3.A. S6.11 reads `WorkspaceConfig::lazy_tool_manifest` and threads it into `ToolRegistry::build_schema_json(..., lazy)` -- when on, every tool entry's `parameters` schema collapses to `{type:"object", additionalProperties:true}` except `describe_tool` itself. S6.13 added an in-stream watchdog: a lambda `check_watchdog()` polls combined `accumulated_reasoning + accumulated_text` chars + steady_clock seconds against `reasoning_max_chars` / `reasoning_max_seconds`; first trip per round flips `out.watchdog_tripped`, broadcasts `IFrontend::on_reasoning_watchdog_tripped(trigger, value)`, and when `reasoning_auto_nudge` is on also sets `out.watchdog_auto_nudge=true` + `cancel_flag_=true` to break the stream cleanly so AgentCore can re-enter the next round with an injected steering message. | `AgentLoop`, `AgentStepResult` |
| `src/agent/slash_commands.h/cpp` | Slash-command tokenizer + dispatcher. Parser is pure (quoted args, key=value, positional mapping, typed coercion via `std::from_chars`; raises `SlashParseError` on malformed input). `ParsedSlashCall` always carries `positional` + `kwargs` for S4.X template substitution alongside the typed `args` JSON. Dispatcher owns execution, `/help` rendering (with Templates (project) / Templates (global) subgroups when a registry is attached via `set_template_registry`), and `complete(prefix)` for autocomplete (each completion now has a `kind = "tool" \| "template"`). Extracted from `AgentCore` in S3.J. | `SlashCommandParser`, `SlashCommandDispatcher`, `ParsedSlashCall`, `SlashCompletion`, `SlashParseError` |
| `src/agent/prompt_templates.h/cpp` | S4.X prompt-template registry. Owns `<workspace>/.locus/prompts/<name>.md` (project) and the global `%APPDATA%/Locus/prompts/<name>.md` (Windows; XDG / `~/.locus/prompts/` on Unix). Per-entry mtime-stamped cache means `list()` / `find()` / `expand()` re-read changed files lazily; `reload()` does a full directory scan (called on workspace open + via the new `/reload` slash). Pure `substitute()` is exposed for tests: `{0}` / `{1}` ... positional, `{key}` named, `{{` / `}}` escapes, missing references rendered as literal placeholders. Frontmatter parser handles `description: ...` + `args: [a, b, ...]`; anything more complex is ignored (forward-compat). Owned by `AgentCore`; consumed in two places -- `AgentCore::try_slash_command` for the expansion path (returns `SlashOutcome::expanded` with the body text) and `SlashCommandDispatcher` (non-owning pointer for `/help` + `complete()`). No execution context inside templates by design -- this is the security model. | `PromptTemplate`, `PromptTemplateRegistry` |
| `src/agent/file_change_tracker.h/cpp` | Thread-safe between-turn file-change tracker (S4.T). Holds `path -> mtime` snapshot + per-turn "agent touched" suppression set. `snapshot()` rebases against `IndexQuery::list_directory`; `diff_since_snapshot()` returns added/modified/deleted paths; `mark_agent_touched()` is called by `ToolDispatcher` for every `edit_file` / `write_file` / `delete_file` so the agent's own writes don't echo back as "external" next turn. `AgentCore` snapshots at construction + end-of-turn and diffs at top-of-turn, prepending `[Files changed since last turn: ...]` to the user message when non-empty. | `FileChangeTracker` |
| `src/agent/mention_parser.h/cpp` | S4.V `@`-mention parser. Pure function `parse_mentions(text)` returns `(start, end, path)` for every `@<path>` token in user input. `@` is recognised only when preceded by start-of-text / whitespace / one of `()[],;:` (the heuristic that keeps `user@example.com` from being treated as an attach intent); path chars are alphanumeric plus `/\._-+~#`; trailing dots are trimmed so "see @src/foo.cpp." doesn't keep the period. Consumed by `ChatPanel`'s submit path (first valid mention -> `agent.set_attached_context`) and by the autocomplete popup's at-cursor detection. | `Mention`, `parse_mentions` |
| `src/agent/auto_commit.h/cpp` | S4.L per-turn auto-commit. Free function `auto_commit_after_turn(workspace_root, prefix, branch, message)` runs a small Win32 sync subprocess pipeline (separate from S4.I `RunCommandTool` -- short-lived `git ...` calls don't need Job Object / timeout overhead): detect `.git/` -> optional branch switch (creating + warning if missing) -> `git add -A` -> `git commit -F <tempfile>` (avoids cmd.exe quoting) -> `git rev-parse --short HEAD`. "Nothing to commit" stderr is treated as `skipped=true`, not failure. Called from `AgentCore::process_message` after the round loop when `change_tracker_->agent_touched_size() > 0` and `WorkspaceConfig::git_auto_commit` is true. | `AutoCommitResult`, `auto_commit_after_turn`, `make_commit_subject` |
| `src/agent/agent_core.h/cpp` | Orchestrator: agent thread + message queue + lifecycle, plus plan-mode (S4.D) state. Composes `LLMContext` (S5.J -- LLM-view state owner) alongside `AgentLoop` / `ToolDispatcher` / `ActivityLog` / `MetricsAggregator` / `SlashCommandDispatcher` / `PromptTemplateRegistry`. Owns the `SessionManager` (shared into `LLMContext` by reference). Conversation-state operations (`history()`, `attached_context()`, `read_current_pre_mutation()`, etc.) are passthroughs into `ctx_` so existing frontend code that called `agent.history()` etc. keeps working. The S5.J refactor split out everything that described "what the LLM sees" from "agent-loop machinery"; `AgentCore` is back to the runner role. **S6.11/12** the SystemPromptAssembly build site (ctor + per-turn rebuild) now reads `lazy_tool_manifest` + `system_prompt_profile` from `WorkspaceConfig` and threads both into `SystemPromptAssembly::build`; the info log line names the profile so cache-key changes are obvious. **S6.13** added `request_commit_now()` (sets `nudge_requested_` + `cancel_requested_` together; idempotent), per-turn `nudges_this_turn_` counter (reset in `process_message`, hard-capped at 2 before the turn aborts with "Agent appears stuck"), and the round-loop branch that distinguishes user-Stop from commit-now-nudge from auto-nudge -- in the nudge cases it clears the cancel flag, appends a synthetic user message "Stop reasoning. Commit to a tool call now, or give a brief final answer.", and continues the loop instead of breaking. | `AgentCore` |
| `src/frontends/cli/cli_frontend.h/cpp` | Terminal frontend: token streaming, y/n/e tool approval, context meter, compaction prompts. | `CliFrontend` |
| `src/main.cpp` | CLI entry point. Arg parsing, logging init, REPL loop, Ctrl+C handler. | `CliArgs` |
| `src/frontends/gui/locus_app.h/cpp` | wxApp entry point. Owns Workspace, LLM, Agent lifetime. Single-instance check. | `LocusApp` |
| `src/frontends/gui/locus_frame.h/cpp` | Main window. wxAuiManager 3-pane layout, status bar, agent-event routing. Delegates menu bar to `MenuController`, status-pane composition to `OpsStatusView`. | `LocusFrame` |
| `src/frontends/gui/menu_controller.h/cpp` | Owns the wxMenuBar and its dynamic submenus (Recent Workspaces, Saved Sessions). All actions run through `Hooks` callbacks supplied by `LocusFrame`. | `MenuController` |
| `src/frontends/gui/ops_status_view.h/cpp` | Composes the right-pane status text from indexing/embedding progress counters. No wx dependencies beyond wxString. | `OpsStatusView` |
| `src/frontends/gui/chat_panel.h/cpp` | Chat UI: wxWebView (HTML/CSS), wxTextCtrl input, context meter footer. Streaming via wxTimer + md4c. S4.D adds a three-state `Chat / Plan / Execute` mode switcher above the input, an in-chat plan bubble (`.msg-plan` with status glyphs + Approve/Reject links via the `locus://` scheme intercepted in `on_webview_navigating`), and a `plan_chip_` next to the context meter. S4.V wires the `@`-mention popup (`set_mention_paths` + `set_on_mention_attach`; `active_mention_at_cursor` detects an `@<prefix>` near the caret; submit-time `parse_mentions` extracts the first valid path and calls back). S5.G adds `set_system_prompt_bubble(const SystemPromptAssembly&)` (renders a collapsed `<details class="msg-system-prompt">` at dom_id 0 with per-section token chips), `on_history_message_added/deleted` slots that maintain the `history_to_dom_` map and inject the hover-reveal X via `addDeleteButton` (CSS `:hover` reveals `.msg-delete-x` with `href="locus://delete-message/<history_id>"`; ChatLinkHandler routes through `wxMessageDialog` confirm + the `on_delete_message_` callback), plus token chips on tool-result bubbles (estimate via `TokenCounter::estimate(display) + 4`). S5.Z task 6 adds `set_compacted_count(int, archive_dir)` -- the new `compacted_chip_` shows `compacted: N` between the commit chip and the preset row, hidden when N == 0; left-click on the chip shells out via `ShellExecuteW(L"open", ...)` on Windows (was `wxLaunchDefaultApplication` -- silently no-oped on directories in some shell configs) and creates the dir on demand so the click works even before the first archive write. S6.13 follow-up added `wxButton* commit_btn_` (named widget `locus.chat.commit_btn`, between Undo and Stop, hidden by default) + `set_commit_now_visible(bool)` (idempotent, only fires `Layout()` on state change) + `set_on_commit_now(callback)`; visibility is toggled by `WxFrontend::on_reasoning_watchdog_tripped/cleared` events and force-hidden on `on_turn_complete` as a belt-and-suspenders. Click invokes the registered callback -- `LocusFrame` wires it to the active tab's `ILocusCore::request_commit_now()`. | `ChatPanel` |
| `src/frontends/gui/mention_popup.h/cpp` | S4.V `@`-mention autocomplete dropdown -- sister of `SlashPopup`. Built once per session over the workspace-relative path list captured at `LocusFrame` setup. Filtering ranks basename-prefix > path-prefix > basename-substring > path-substring; visible list capped at 50 rows. Same `move_up`/`move_down`/`selected_path`/`on_accept`/`on_dismiss` shape as `SlashPopup`. | `MentionPopup` |
| `src/frontends/gui/markdown.h/cpp` | md4c wrapper: markdown -> HTML (GitHub-flavored dialect). | `markdown_to_html()` |
| `src/frontends/gui/locus_tray.h/cpp` | System tray icon. State display (idle/active/error), right-click menu, minimize-to-tray. | `LocusTray` |
| `src/frontends/gui/tool_approval_panel.h/cpp` | Tool approval: AUI bottom pane with JSON args (Scintilla), approve/modify/reject buttons, ask_user mode. | `ToolApprovalPanel` |
| `src/frontends/gui/wx_frontend.h/cpp` | IFrontend thread bridge: agent thread callbacks -> wxThreadEvent -> UI thread. | `WxFrontend` |
| `src/frontends/gui/metrics_view.h/cpp` | "Metrics" tab inside `ActivityPanel` (S4.S). Renders `MetricsAggregator::aggregates()` -- text totals plus two pure-`wxDC` spark bars (turn duration, total tokens). Refreshes on a 1 s timer + on every appended activity event. "Export JSON.../CSV..." buttons pop a `wxFileDialog` so the user picks the destination. | `MetricsView` |
| `src/frontends/gui/compaction_dialog.h/cpp` | Context compaction modal: strategy B/C radio, N-turns slider, message preview, before/after token counts. | `CompactionDialog`, `CompactionChoice` |
| `src/frontends/gui/file_tree_panel.h/cpp` | File tree sidebar: wxTreeCtrl with lazy-loading from IndexQuery, index stats, re-index gauge. | `FileTreePanel` |
| `src/frontends/gui/settings_dialog.h/cpp` | Settings modal: LLM endpoint/model/temperature/context, index exclude patterns. | `SettingsDialog` |
| `src/frontends/gui/notification_sounds.h/cpp` | Per-event sound alerts (Windows-only). Maps four `Kind`s to distinct `PlaySoundW` aliases -- `SystemExclamation` (tool approval), `SystemQuestion` (ask_user), `SystemAsterisk` (turn complete), `SystemHand` (compaction). `play(kind, cfg, frame)` is a no-op when the matching `WorkspaceConfig::Notifications` flag is off OR when `only_when_unfocused` is set and `frame` (or any of its owned modals -- checked via `GetAncestor(fg, GA_ROOTOWNER)`) is the foreground window. Called from `LocusFrame::on_agent_tool_pending` (split between the ask_user and approval-modal branches), `on_agent_turn_complete`, and `on_agent_compaction`. `winmm.lib` linked behind `$<$<PLATFORM_ID:Windows>:winmm>` so non-Windows builds compile as no-ops. | `notification_sounds::Kind`, `notification_sounds::play` |
| `src/frontends/gui/settings/notifications_settings_panel.h/cpp` | Settings tab: four per-event sound toggles + one "Only when the Locus window isn't focused" gate. Writes to `WorkspaceConfig::Notifications`. Tab id `ui_names::kSettingsTabNotifications`. | `NotificationsSettingsPanel` |
| `src/frontends/gui/memory_bank_panel.h/cpp` | S5.K dockable Memory Bank panel. AUI-docked on the right; default hidden, toggled via View > Memory Bank (Ctrl+M). Pure view over `MemoryStore` -- store is the source of truth, panel renders snapshots. Toolbar (search-as-you-type, source / tag / pinned-only filters, "Show deleted" toggle, "Clear"), `wxDataViewListCtrl` list, edit-in-place detail pane (content + tags + pinned + Save), and bulk-op buttons (Delete / Pin/Unpin / Add tags). Right-click context menu on rows. Search + filter changes go through a 200 ms debounce timer. Live updates: `on_activity_event` re-renders on `memory_added` / `memory_deleted` / `memory_searched` plus any `tool_result` whose summary names `add_memory` / `search_memory` (the agent-tool path emits the latter; only slash paths emit the former, so the panel watches both). When `capabilities.memory_bank` flips off mid-session, the panel auto-hides and the View menu item greys out via `LocusFrame::update_memory_bank_menu_state`. v1 deferred items (cross-linking to chat bubbles, zip import/export, "N new since last looked" badge) are intentionally absent -- the stage doc lists them as polish, not load-bearing. | `MemoryBankPanel` |
| `src/frontends/gui/autostart.h/cpp` | Windows startup-on-login via HKCU Run key (opt-in). | `is_autostart_enabled()`, `set_autostart_enabled()` |
| `src/frontends/gui/ansi_parser.h/cpp` | S5.B stateful, partial-sequence-safe ANSI escape parser. Lives in `locus_core` (no wx dependency) so unit tests link against it cleanly. Handles SGR (foreground 30-37/90-97, background 40-47/100-107, bold/dim/reset), erase-in-line / erase-in-display, and bare CR -> erase-line; recognises + drops 256-colour / truecolour / cursor-position / OSC. Output is a vector of `AnsiEvent` (text run + style, or one of the erase commands); state machine survives split-CSI and split-`ESC[` across chunk boundaries. | `AnsiParser`, `AnsiStyle`, `AnsiEvent` |
| `src/frontends/gui/terminal_panel_state.h/cpp` | S5.R wx-free per-tab terminal model. Owns the canonical view that `TerminalPanel` renders: per-sub-tab event log (parsed `AnsiEvent`s), `AnsiParser` instance, badge state, exit-status fields. Implements `IProcessSink` -- worker threads write into mutex-protected `pending_text_` + `pending_lifecycle_` queues and return. The UI thread's flush timer (in the widget) calls `drain_pending` to swap queues out, then `parse_and_append` to walk each chunk through the per-sub-tab parser and append to the canonical event log. Scrollback cap drops oldest events at event granularity (slightly over-trims; acceptable for a soft scrollback bound). Lives in `locus_core` alongside `AnsiParser`; the auto-show predicate `should_auto_show_terminal(...)` is exposed here as a free function so locus_tests can verify it without wx. | `TerminalPanelState`, `TerminalTabState`, `should_auto_show_terminal` |
| `src/frontends/gui/terminal_panel.h/cpp` | S5.B + S5.R live terminal panel widget. AUI-docked at the bottom of the main frame (default hidden; View > Terminal / Ctrl+`). S5.R turned the widget into a pure view over a per-tab `TerminalPanelState`: `set_state(...)` (called from `LocusFrame::on_notebook_page_changed`) clears the inner `wxNotebook` and rebuilds it from the new state's sub-tab list, replaying the canonical `AnsiEvent` log into each STC. The flush `wxTimer` drains the **currently bound** state, parses pending chunks via `state_->parse_and_append`, and writes styled bytes to the STC pages. Inactive states accumulate raw bytes silently; on activation the next tick processes the backlog. Per-tab `wxStyledTextCtrl` with a pre-populated style table for the 16-colour palette + bold variants; scrollback capped at 10000 lines. Kill / copy / clear right-click actions run through a `KillHandler` callback the frame supplies (always routed to the active tab's `ProcessRegistry`). S5.Z task 4 added a per-bg-tab single-line `wxTextCtrl(wxTE_PROCESS_ENTER)` (`stdin_input`) docked below the STC; Enter echoes the typed line into the scrollback in dim-grey style and forwards it to a `StdinHandler` callback (sister of `KillHandler`). Sync `Run` tab leaves the stdin row off because the dispatcher waits synchronously on exit. | `TerminalPanel` |

**Test files** follow `tests/test_<topic>.cpp` -- one per subsystem, tagged by stage. `tests/support/shared_embedder.h/cpp` lazily loads `bge-small-en-v1.5-Q8_0.gguf` once per process and installs a `Workspace::set_embedder_provider` hook via a Catch2 EventListener; the listener clears the hook + drops the cached model at `testRunEnded`. Without this the suite reloaded bge-m3 (~6 s x 38 Workspace-creating tests = ~240 s); with it the full suite runs in ~21 s.

**Integration tests** live in `tests/integration/` -- a separate `locus_integration_tests` executable that drives `AgentCore` against a live LM Studio LLM end-to-end. Manual-only (not in `ctest`). See [tests/integration/README.md](tests/integration/README.md) for what each tag area covers.

**Retrieval eval harness (S4.K)** lives in `tests/retrieval_eval/` -- a separate `locus_retrieval_eval` executable that opens a workspace, runs every gold query in `queries.json` through `search_text` / `search_semantic` / `search_hybrid`, and emits recall@K + MRR + nDCG@10 to `results.md`. Manual-only (full index + bge-m3 + queue drain is too heavy for `ctest`); pure-math metric tests in [tests/test_retrieval_eval.cpp](tests/test_retrieval_eval.cpp) DO run with the rest of the suite. See [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md).

**Data flow**: `main` -> `Workspace` (owns `Database`, `ExtractorRegistry`, `FileWatcher`, `Indexer`, `IndexQuery`; implements `IWorkspaceServices`) -> `LLMContext` (S5.J: owns `ConversationHistory` + `SystemPromptAssembly` + `ContextBudget` + `FileChangeTracker` + optional `CheckpointStore` + attached-context slot + session/turn ids; shares `SessionManager` by reference) -> `AgentCore` (owns `SessionManager` + agent thread + message queue + plan-mode state; composes `LLMContext` as `ctx_`; bridges `ILLMClient` + `IToolRegistry` + `IWorkspaceServices&`) -> frontends receive events via `IFrontend` through `FrontendRegistry`.

**Adding a new file format extractor**: Create `src/extractors/<format>_extractor.h/cpp` implementing `ITextExtractor`. Register it in `Workspace::Workspace()` (`workspace.cpp`) with `extractors_->register_extractor(".ext", ...)`. Add the `.cpp` to `locus_core` in `CMakeLists.txt`. No changes to `src/index/indexer.cpp` needed.

---

## Document Map

| File | When to read |
|---|---|
| [CONTRIBUTING.md](CONTRIBUTING.md) | Code style, build/run/test instructions for humans |
| [tests/integration/README.md](tests/integration/README.md) | Manual LLM-driven integration test suite -- tags, harness design, update rules |
| [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md) | Retrieval eval harness -- gold queries, recall/MRR/nDCG, baseline + regression check |
| [tests/ui_automation/README.md](tests/ui_automation/README.md) | S5.L UI Automation driver -- JSON scripts, step ops, naming conventions, focus-stealing caveat |
| [tests/ui_automation/AGENTIC_TESTING.md](tests/ui_automation/AGENTIC_TESTING.md) | **Agentic testing** -- interactive TCP/JSON driver for QA-LLM-led end-to-end exploration. Read this when the user asks you to "do agentic testing" or "test how Locus handles X" with a real LLM in the loop. |
| [roadmap.md](roadmap.md) | Roadmap index -- milestones M0-M6 with one-table summaries. Real detail lives in `roadmap/`. |
| [roadmap/M0.md](roadmap/M0.md), [M1.md](roadmap/M1.md), [M2.md](roadmap/M2.md) | Completed milestones -- full task lists, one file per milestone |
| [roadmap/M3/](roadmap/M3/) | M3 Refactoring done -- one file per stage (S3.A-S3.L) |
| [roadmap/M4/](roadmap/M4/) | M4 Agent Quality -- one file per stage (S4.A-S4.Y) |
| [roadmap/M5/](roadmap/M5/) | M5 Polish, UX & Performance -- one file per stage (S5.A...) |
| [roadmap/M6/](roadmap/M6/) | M6 Connected & Misc -- one file per stage (S6.1-S6.8) |
| [roadmap/backlog/README.md](roadmap/backlog/README.md) | Unscheduled tasks + parked drafts (e.g. S4.E LSP) -- items recognised as worth doing but not yet promoted to a milestone |
| [test-workspaces.md](test-workspaces.md) | Three concrete test cases -- read when scoping features |
| [vision.md](vision.md) | Understanding the *why* behind any design decision |
| [DIFFERENTIATORS.md](DIFFERENTIATORS.md) | Who this is for, what makes it different |
| [requirements.md](requirements.md) | Full feature list -- reference when scoping work |
| [architecture/overview.md](architecture/overview.md) | System component map, data flow, context strategy |
| [architecture/agent-loop.md](architecture/agent-loop.md) | AgentCore turn orchestration -- threading, phases, sequence diagrams, M4 hook points |
| [architecture/threading-model.md](architecture/threading-model.md) | Thread inventory, shared-resource protectors, cross-thread invariants, checklist for new threads |
| [architecture/tool-protocol.md](architecture/tool-protocol.md) | ITool interface, approval flow, adding tools |
| [architecture/workspace-index.md](architecture/workspace-index.md) | Index subsystem: schema, update strategy, query API |
| [architecture/tech-stack.md](architecture/tech-stack.md) | Tech stack -- decided choices and future frontend options |
| [architecture/web-retrieval.md](architecture/web-retrieval.md) | Web RAG: fetch -> index -> search pipeline, token budget strategy |
| [architecture/mcp.md](architecture/mcp.md) | MCP integration: mcp.json schema, namespacing, approval/trust, lifecycle, result cap |
| [architecture/decisions/](architecture/decisions/) | ADRs -- *why* behind non-trivial architectural shifts (one file per decision) |

---

## Key Architecture Principles (Non-negotiable)

1. **Index is never written by the LLM** -- only deterministic algorithmic updates
2. **Tools over context stuffing** -- agent fetches what it needs, never pre-loads
3. **Paginate everything** -- no full-file dumps into context
4. **Operating transparency** -- User sees tool calls, LLM/context state and other operations - nothing executes without user seeing it, everything is transparent
5. **Context window is precious** -- always summarize tool results before injection
6. **Instant UI feedback** -- streaming responses, no blocking main thread ever

---

## Development & Testing Protocol

**Test workspace**: `d:\Projects\AICodeAss\` -- this folder IS WS1. Locus always runs against
itself during development. What Locus indexes is exactly what is visible here.

**After every completed stage:**
1. `cmake --build build/release --config Release` (full build)
2. `build/release/Release/locus.exe d:/Projects/AICodeAss -verbose` (or `locus_gui.exe` for GUI work)
3. Exercise the stage's features
4. Quit the exe
5. Read `.locus/locus.log` -- verify expected behavior, catch errors
6. Fix issues before moving to the next stage

For a unit-test sweep: `build/release/tests/Release/locus_tests.exe`. Full command
reference is in the [Build & Test Commands](#build--test-commands) section above.

**`-verbose` flag** (implemented in S0.1):
- spdlog level drops to `trace` (vs default `info`)
- All SQL queries logged
- Every tool call logged with full args and result
- LLM token stream logged
- Index update events logged

This protocol means every stage is validated before the next begins.
No accumulated debt, no "we'll debug it later."

**Integration tests (`tests/integration/`, manual, LLM-driven):**
- **Update them whenever any user-observable agent behaviour changes** -- that includes tools, extractors, the agent loop, the approval gate, the slash-command dispatcher (built-in slashes too: `/undo`, `/metrics`, `/export_metrics`, `/compact`, `/save`), the LLM stream decoder (`finish_reason` / refusal / mid-stream errors / tool-call validation), checkpoint and undo behaviour, telemetry surfaces (`MetricsAggregator`, `/metrics`, `/export_metrics`), and conversation/session state transitions. Schema changes, new tool, retired tool, changed arg shape, new extractor, new approval path, new slash command, new stream-decode event, new max_tokens / context-limit / timeout behaviour all need matching test coverage in the relevant `test_int_*.cpp`. Add a new `TEST_CASE` tagged `[integration][llm][<topic>]`; don't silently skip. If a feature can be exercised without an LLM round-trip (slash-only, deterministic), it still belongs in this suite tagged `[integration]` (drop `[llm]`) so the manual sweep runs it.
- **Do NOT run them by default.** They need a live LLM, take minutes, and are not part of the per-stage verification protocol above. Only run when explicitly asked ("run the integration tests", "run [search] integration").
- **Run inline (no `-console`).** stdout / stderr come back through the subprocess pipe so Claude can read failures immediately and root-cause without hunting through `.locus/integration_test.log`. Use a `grep -vE` filter to suppress the llama.cpp boot block when only Catch2 / harness lines matter. `-console` only when a human at the desktop wants the foreground console pop-out; in that mode the subprocess capture comes back empty (stdout is redirected into the new window).
- **Minimum verified model: Gemma 4 E4B @ 8k context.** Larger / more capable models are fine. Any model without tool-calling support will fail most cases.
- See [tests/integration/README.md](tests/integration/README.md) for the full run matrix, env vars, and harness design notes.

**UI automation scripts (`tests/ui_automation/`, manual, Windows-only):**
- **Add a script whenever a stage ships an addressable GUI surface** -- new wx panel / dialog / menu item / accelerator / chat-footer chip / settings sub-control / popup / approval flow / chat slash-rendering. The rule mirrors the integration-tests rule: any new user-observable wxWidgets surface gets a smoke-level UIA assertion the same commit that introduces it, so the next stage's GUI work doesn't silently break it. Pure agent-core / index / LLM behaviour with no widget belongs in unit or integration tests instead, not here.
- **Naming first.** A script can only address widgets named in [src/frontends/gui/ui_names.h](src/frontends/gui/ui_names.h). Add the `kFoo` entry there and the matching `widget->SetName(ui_names::kFoo); gui::apply_locus_accessible_name(widget);` call in the owning `*_panel.cpp` / `*_dialog.cpp` -- same pattern S5.L established. Widget naming is strictly additive; no behaviour change.
- **Prefer deterministic over LLM-dependent.** Most GUI surfaces can be exercised without a live LLM (menus, dialogs, settings persistence via re-launch, slash popup, file-on-disk side effects via `assert_file_exists`). Only reach for an LLM-dependent script (like `plan_mode.json`, `tool_approval_dialog.json`, `inline_diff_render.json`, `terminal_panel_run.json`) when the surface genuinely only renders during a real agent turn (approval gate, inline diff, terminal panel populated by `run_command`). LLM-dependent scripts inherit the existing "fails loudly if LM Studio isn't reachable" behaviour; document the model requirement in the script's `description` field.
- **Setup knobs the harness ships with**: `setup.workspace` (`"tmp"` is default, fresh dir per run), `setup.allow_first_time_prompts` (let first-open modals fire), `setup.env` with `{workspace}` substitution (e.g. point `LOCUS_GLOBAL_DIR` at a tmp dir), `setup.tool_approvals_override` (force a tool to `ask` so the approval gate fires). Reach for these instead of editing the harness.
- **Do NOT run them by default.** Like the integration tests, UIA scripts pop real windows on the desktop session, steal focus, and intercept keystrokes. Run only when explicitly asked ("run the UIA tests", "run smoke + settings_tour"). When you do run them, ask the user first if they're at the keyboard -- the focus-stealing surprise costs more than the regression it would catch.
- **What UIA can't see.** Reading Scintilla content (tool-approval JSON pane, terminal-panel tabs) is unreliable; assert surfaces (visibility, parent presence, tab existence) and use the chat WebView as a parallel signal where the same content is mirrored. `wxChoice` `CBS_DROPDOWNLIST` selection-by-key doesn't reliably fire `wxEVT_CHOICE`; assert the dropdown is reachable and defer the modal-firing assertion to the matching manual test plan. `wxPopupTransientWindow` UIA visibility varies; observe popup side-effects on the parent control instead. No right-click op -- file-tree context menus stay in `tests/manual/`.
- See [tests/ui_automation/README.md](tests/ui_automation/README.md) for the full step-op reference, naming conventions, focus-stealing caveat, and the bundled-scripts table.

**Agentic testing (`locus_ui_tests --agentic`, S5.Z task 9):**
- Same harness binary, different mode. The scripted runner replays canned `.json`; the agentic mode launches Locus once, opens a TCP/JSON service on `127.0.0.1:<port>`, and lets a QA-LLM (a separate Claude session) drive Locus interactively. The point: catch failure modes a deterministic script can't anticipate -- "build a small C++ minigame", "small-LLM tool-calling robustness", "what does the local agent do when its first plan doesn't work".
- **When the user asks for "agentic testing" / "test how Locus does X" / "test against a small LLM" -- read [tests/ui_automation/AGENTIC_TESTING.md](tests/ui_automation/AGENTIC_TESTING.md) first.** That document is the entry point. It carries: launch flags, the PowerShell helper for the wire protocol, the full op catalog (existing UIA primitives + `submit_chat` / `read_chat` / `wait_for_text_stable` / `wait_for_log_contains` / `list_workspace` / `read/write_workspace_file` / `read_locus_log` / `get_chat_status` / `list_named_widgets`), recipes for the canonical scenarios, and the findings-report template. Don't try to drive the server cold -- the helper conventions matter and the op shapes aren't memorised.
- Findings end the session as `findings.md` in the agentic output dir. The user reads it, fixes, then asks for another run -- often with a different model.

**Manual test plans (`tests/manual/`):**
- Whenever a stage ships a user-facing feature (GUI panel, new workflow, anything a non-developer can click through), write a QA-style manual test plan and drop it in `tests/manual/<feature-name>.md`. Filename is the feature as users see it (`mcp.md`, `plan-mode.md`, `checkpoints.md`), not the stage code (`S4.G`).
- Audience is QA already familiar with the project. Don't restate well-known project-level facts -- how to launch Locus, what a workspace is, that only one Locus instance can attach to a workspace at a time, the standard `LM Studio at 127.0.0.1:1234 with a tool-calling model` setup. Skip Prerequisites entirely if there's nothing test-specific to call out.
- **Few tests, each broad.** Aim for 1-3 tests per plan; add a fourth only when a behaviour genuinely doesn't fit. Fold related checks into one happy-path walkthrough rather than splitting them into ceremony-heavy mini-tests. A single test that exercises round-trip + frontmatter + substitution + precedence is more useful than four tests with separate Setup/Cleanup blocks. Some plans (e.g. [mcp.md](tests/manual/mcp.md), [memory-bank.md](tests/manual/memory-bank.md)) predate this guidance and are larger -- bring new plans down to ~80-150 lines instead.
- Tight prose. No "Audience" / "Estimated time" / "What you're looking at" preamble unless a non-trivial concept genuinely needs framing first. No "Cross-test gotchas" appendix duplicating points already in the steps. Numbered steps with one Expected outcome per test; Hard fails only when a result is ambiguous and you want to spell out what's actually broken.
- No build commands, no `src/` paths, no code references. Steps are GUI clicks or text-file edits.
- Add a row to [tests/manual/README.md](tests/manual/README.md)'s index table the same commit so the new plan is discoverable.
- These plans complement scripted suites (unit + integration + UI automation) -- they catch what those structurally don't reach: drag-and-drop, focus order, file-dialog interactions, third-party-software workflows, Scintilla content rendering, right-click context menus, visual layout regressions, and anything else UIA can only assert at the surface-presence level.

**After every completed stage/task -- mandatory bookkeeping:**
1. Update `roadmap.md`: mark completed tasks `[x]`, add done to the stage header
2. Add the completed stage code to the milestone line in `CLAUDE.md`'s "Current Stage" section. If the stage introduces a load-bearing cross-cutting invariant (the kind of thing a refactor in a neighbouring file could silently break), add a one-liner to "Key invariants". Update Code Map row(s) when a file's responsibilities actually changed. Per-stage narrative belongs in the per-stage doc, not CLAUDE.md -- but a sharp invariant note here is fine.
3. **ADRs are rare.** Only write one if the stage crossed a system boundary
   (technology swap, subsystem reshape, ownership/threading/data-format change) **and**
   rejected a reasonable alternative for a non-obvious reason **and** that reasoning
   isn't already captured in the stage doc or an inline comment. Interface
   refactors, file moves, and ergonomic choices do **not** qualify -- the stage doc's
   "Implementation notes" section is the right home for those. See
   [architecture/decisions/README.md](architecture/decisions/README.md) for the full
   bar. When in doubt, don't write one -- ask first.
4. Do this immediately after verification passes, before moving on

---

## Token Efficiency Notes

When working on this project's own code:
- Read files before editing (read the specific section, not whole file)
- Use Grep for finding specific patterns before reading
- The architecture docs are the source of truth -- don't re-derive what's written there
- This CLAUDE.md file should stay concise -- it's a quick-scan reference, not a second vision doc
