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

**M4 — Agent Quality in progress.** S4.A / S4.P / S4.J / S4.B / S4.K / S4.I / S4.S / S4.M / S4.T / S4.N / S4.U / S4.D / S4.G / S4.L / S4.F / S4.W done. **S4.R Phase 1 (engine + tools + slash) done** -- Settings UI tab + chat right-click "Save as memory" deferred to a Phase 2 that lands alongside the rest of the M5 UX polish. **S4.C parked to backlog** -- the auto-verify-loop premise breaks down on small local LLMs (build-error retry thrash); reactivation gate captured in [roadmap/backlog/S4.C-auto-verify.md](roadmap/backlog/S4.C-auto-verify.md). **M3 refactoring fully complete** -- all 12 stages: S3.A (`AgentCore` split into `AgentLoop` / `ToolDispatcher` / `ActivityLog` / `ContextBudget`; agent sources moved to `src/agent/`), S3.B (`ILLMClient` split into `OpenAiTransport` + `IStreamDecoder` + `OpenAiDecoder` + `TokenCounter`, plus S4.N decoder stubs and S4.Q `LlmRouter` skeleton; sources moved to `src/llm/`), S3.C (`IWorkspaceServices`), S3.D (`Indexer` split into `TreeSitterRegistry` + `ISymbolExtractor` registry + `IndexerStatements` RAII; index sources moved to `src/index/`), S3.E (`tools.cpp` split into `src/tools/` by family -- shared / file / search / index / process / interactive), S3.F (`WatcherPump` + `LocusFrame` split), S3.G (`LocusSession` bundle), **S3.H (`src/` layering: every file now sits under `src/{core,agent,llm,index,tools,frontends/cli,frontends/gui,extractors}/`; `main.cpp` is the only thing left at the `src/` root)**, S3.I (`architecture/threading-model.md` + `ConversationHistory::assert_owner_thread()` fence, `ConversationOwnerScope` RAII), S3.J (`SlashCommandParser` + `SlashCommandDispatcher` extracted), S3.K (`architecture/agent-loop.md`, `architecture/decisions/` ADR trail), S3.L (tool-catalog hygiene: unified `search(mode=…)`, `ITool::available()` / `visible_in_mode()` gating, per-turn manifest-token warning).

**S4.A (Diff-Based Editing) done.** One `edit_file` tool in [src/tools/file_tools.cpp](src/tools/file_tools.cpp): takes `{path, edits: [{old_string, new_string, replace_all?}, ...]}`. A single edit is an array of one; multiple edits apply atomically (all succeed or none are written) with later edits seeing earlier results. Uniqueness is enforced per edit. Writes go through temp-file + rename for atomicity. Process-wide `ReadTracker` in [src/tools/shared.h](src/tools/shared.h) forces a prior `read_file` on the same path before edits are accepted — mirrors Claude Code's hallucinated-edit mitigation. `create_file` folded into `write_file` with an `overwrite` flag (default `false`); refusing to replace an existing file is now the default, set `overwrite=true` explicitly to opt in. System prompt updated to prefer `edit_file` over `write_file`. Approval panel renders `edit_file` as a red/green unified-diff inside the existing Scintilla pane; `Modify` flips to raw JSON and back. Kept tool set slim (9 built-ins, not 11) to keep the manifest small for local LLMs.

**S4.P (Regex search) done.** New `mode=regex` on the unified `search` tool — in-process `std::regex` (ECMAScript) over every indexed, non-binary file, with `pattern` / `path_glob` / `case_sensitive` / `max_results` params. Line-numbered output with ±1 line context. File enumeration goes through `IndexQuery::list_directory("", huge_depth)` so the indexer's exclude patterns are already respected — no duplicate filtering. `SearchRegexTool` lives in [src/tools/search_tools.h/cpp](src/tools/search_tools.h) alongside the other per-mode classes; it isn't registered directly — `SearchTool::execute` delegates on `mode=regex`. `query` on the `search` tool is now optional at schema level; each mode enforces its own required field at runtime. Still 9 built-in tools.

**S3.D (`Indexer` split) done.** Tree-sitter language map and prepared statements lifted out of `Indexer`. New [src/index/tree_sitter_registry.h/cpp](src/index/tree_sitter_registry.h) owns the `TSParser*` and the `name → TSLanguage*` map (9 grammars); intended to be shared with S4.M `ast_search`. New [src/index/symbol_extractor.h/cpp](src/index/symbol_extractor.h) defines `ISymbolExtractor` returning `vector<ExtractedSymbol>` (kind/name/signature/parent_name/line span) plus a `SymbolExtractorRegistry`; the shared two-level walk + `extract_name_from_node` helper live on a private `RuleBasedSymbolExtractor`. Per-language `.cpp` files under [src/index/symbol_extractors/](src/index/symbol_extractors/) hold only their `SymbolRule` table and a `make_<lang>_symbol_extractor()` factory (cpp / python / js_ts / go / rust / java / csharp); `register_builtin_symbol_extractors` wires all seven languages into the `Indexer`'s registry. New [src/index/prepared_statements.h/cpp](src/index/prepared_statements.h) `IndexerStatements` RAII holder consolidates all 14 sqlite_stmt pointers (10 main.db + 4 vectors.db) into one ctor/dtor pair — `Indexer::~Indexer()` is now a one-liner trace log. Existing `indexer.{h,cpp}` / `index_query.{h,cpp}` / `chunker.{h,cpp}` / `glob_match.h` moved into `src/index/`; all 17 includers updated. DB schema unchanged; WS1 cold-reindex matches pre-refactor totals (284 files / 1394 symbols / 612 headings). See [roadmap/M3/S3.D-indexer-split.md](roadmap/M3/S3.D-indexer-split.md).

**S3.G (`LocusSession` bundle) done.** New [src/core/locus_session.h/cpp](src/core/locus_session.h) owns the `Workspace` + `ILLMClient` + `ToolRegistry` + `AgentCore` lifecycle as a single object. Constructor wires them in order (workspace → LLM client → server probe → tools → agent_core, then `agent->start()`); destructor calls `agent_->stop()` first so the agent thread joins before any `unique_ptr` member is freed, then unwinds in reverse declaration order. LLM-config layering happens in one place: defaults → `.locus/config.json` → caller seed (CLI / GUI args) → live `query_model_info`. The CLI now respects `.locus/config.json` like the GUI always has. `main.cpp` shrunk ~70 lines and `LocusApp` shrunk ~120 lines (workspace switch is `frame_->Destroy(); session_.reset(); spawn_session(...)`); future M4 subsystems (LSP, MCP, process registry) slot in as more `unique_ptr` members on `LocusSession` with zero frontend changes.

**S4.B (Checkpoint & Undo) done.** Pre-mutation snapshots for `edit_file` / `write_file` / `delete_file` land under `.locus/checkpoints/<session_id>/<turn_id>/` (manifest in `checkpoint.json`, byte-for-byte file copies in `files/<rel_path>`). New [src/agent/checkpoint_store.h/cpp](src/agent/checkpoint_store.h) owns the on-disk layout; `ToolDispatcher::set_turn_context` arms snapshotting per agent turn (slash-command direct dispatch deliberately bypasses — explicit user action). `AgentCore` generates a process-lifetime session id at construction, bumps a turn counter on each user message, and drops the empty turn dir at end-of-turn so `/undo` doesn't see phantom turns. New `ILocusCore::undo_turn(turn_id=0)` (0 → most recent), exposed in the GUI as a chat-footer **Undo** button and on every frontend as the `/undo [turn_id]` slash command. GC keeps the most-recent 50 turns per session (configurable); `reset_conversation` drops the whole checkpoint session and rolls a fresh id. 1 MB per-file cap — oversized files are recorded in the manifest with `skipped=true` so the user sees them in the restore summary and reaches for git instead. Saved conversation sessions are deliberately decoupled from checkpoint sessions (different identities, different lifetimes) — see [roadmap/M4/S4.B-checkpoint-undo.md](roadmap/M4/S4.B-checkpoint-undo.md) for the rationale.

**S4.S (Telemetry & Agent Performance Metrics) done.** New thread-safe [src/agent/metrics.h/cpp](src/agent/metrics.h) `MetricsAggregator` owned by `AgentCore` next to `ActivityLog` / `ContextBudget`. Per-turn samples + per-tool histogram + retrieval hit rate; `AgentLoop` records `(usage, stream_ms)` per LLM step, `ToolDispatcher` records `(name, success, ms, content_size)` per call, and parses retrieval result counts out of search-tool result headers via `parse_search_result_count`. Saved-session JSON now carries a top-level `"metrics"` key (`SessionManager::save` grew an optional `extras` parameter). Two built-in slash commands handled directly in `AgentCore::try_slash_command`: `/metrics` (one-shot text summary) and `/export_metrics [json|csv]` (writes to `.locus/metrics/`). GUI: `ActivityPanel` is now a 2-tab `wxNotebook` (Log + Metrics); the new [src/frontends/gui/metrics_view.h/cpp](src/frontends/gui/metrics_view.h) `MetricsView` renders the aggregates + two `wxDC` spark bars (turn duration, total tokens) with no external charting lib, refreshes on timer + per-event, and offers "Export JSON…/CSV…" file-dialog buttons. Tool catalog unchanged (still 13). 12 new unit tests in [tests/test_metrics.cpp](tests/test_metrics.cpp) tagged `[s4.s][metrics]`.

**S4.I (Background / Long-Running Commands) done.** New [src/tools/process_registry.h/cpp](src/tools/process_registry.h) owns a per-workspace map of `BackgroundProcess` instances; each child is spawned under a Win32 Job Object (`KILL_ON_JOB_CLOSE`) so terminating the registry kills the whole tree (including grandchildren `cmd /c` spawns). One reader thread per process drains stdout+stderr into a `std::string` capped at `WorkspaceConfig::process_output_buffer_kb` (default 256 KB); monotonic `total_bytes_received` + `bytes_dropped` let `read_output(since_offset?)` tell the LLM exactly how many bytes the ring evicted. Four new tools live in [src/tools/process_tools.h/cpp](src/tools/process_tools.h) — `run_command_bg`, `read_process_output`, `stop_process`, `list_processes` — gating themselves off (`available()=false`) when the workspace has no registry. `IWorkspaceServices::processes()` is the access point; `Workspace` owns the registry and tears it down before the watcher pump so background processes can't outlive the workspace. Tool count rose from 9 to 13. The system prompt now distinguishes synchronous `run_command` (terminating commands -- builds, tests, scripts; default 30-minute cap matching Pi) from `run_command_bg` (servers, watchers, anything that doesn't terminate on its own). UI panel + tray badge deferred — tool calls already surface in the existing activity log + chat. See [roadmap/M4/S4.I-background-commands.md](roadmap/M4/S4.I-background-commands.md).

**S4.K (Retrieval Evaluation Harness) done.** New manual-only [`locus_retrieval_eval`](tests/retrieval_eval/main.cpp) binary opens a workspace, waits for the embedding queue to drain, and runs every gold query in [tests/retrieval_eval/queries.json](tests/retrieval_eval/queries.json) (80 entries against WS1 itself) through `search_text` / `search_semantic` / `search_hybrid`, computing recall@{1,3,5,10}, MRR, and nDCG@10. Aggregate + per-query report goes to [tests/retrieval_eval/results.md](tests/retrieval_eval/results.md); baseline lives at `tests/retrieval_eval/baseline.json` (committed via `--write-baseline`). On normal runs, exit code 1 fires when any metric drops more than `--tolerance` (default 5%) vs. the baseline. **Not wired into `ctest`** — the harness needs a fully indexed workspace + bge-m3 (~600 MB) + queue drain (tens of seconds), too heavy for "every build". Pure-math metric tests in [tests/test_retrieval_eval.cpp](tests/test_retrieval_eval.cpp) DO run with the rest of the suite. Activity-log replay is wired as `--curate-from <sessions_dir>` ([tests/retrieval_eval/curate.cpp](tests/retrieval_eval/curate.cpp)) — walks `.locus/sessions/*.json`, extracts every search-tool invocation from saved conversations, prints a `queries.json` template with empty `expected_files` arrays for hand-filling. See [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md) for the run protocol.

**S4.T (File-Change Awareness Between Turns) done.** New thread-safe [src/agent/file_change_tracker.h/cpp](src/agent/file_change_tracker.h) `FileChangeTracker` holds a `path -> mtime` snapshot plus a per-turn "agent touched" suppression set; both `snapshot()` and `diff_since_snapshot()` source from `IndexQuery::list_directory("", huge_depth)` (the same enumeration S4.P regex search uses), so the indexer's exclude patterns are respected for free. `AgentCore` owns the tracker as a `unique_ptr` member: takes the initial snapshot at construction (so the first user turn doesn't see every indexed file as "new"), diffs at the top of each user turn and prepends `[Files changed since last turn: a, b, c (deleted)]\n\n` to the user's message before it lands in history (matching RooCode/Cline rather than emitting an extra system message — survives compaction and saved-session reload naturally), then re-snapshots at end-of-turn. `ToolDispatcher::set_change_tracker()` wires the dispatcher to the tracker; the existing `maybe_snapshot()` (which already detects `edit_file` / `write_file` / `delete_file` for S4.B checkpoints) gained a second sink that calls `mark_agent_touched(rel_path)` for the same set, so the agent's own writes don't echo back. `clear_agent_touched()` runs at the start of every user turn. Configurable via `WorkspaceConfig::notify_external_changes` (default true; lives under `agent.notify_external_changes` in `.locus/config.json`). Diff caps at 50 entries per bucket so a thousand-file change doesn't flood the prompt. Slash commands deliberately bypass — they don't go through the dispatcher and are explicit user actions. Watcher-debounce caveat: edits made within the 1.5 s quiet period before a prompt show up next turn rather than this one (acceptable; per-turn synchronous flush would block the agent thread). 9 new unit tests in [tests/test_file_change_tracker.cpp](tests/test_file_change_tracker.cpp) tagged `[s4.t][file-change-tracker]`.

**S4.M (Tree-sitter structural search) done.** New `mode=ast` on the unified `search` tool. Caller supplies `language` (one of the nine grammars in `TreeSitterRegistry`) and a Tree-sitter S-expression `query` (e.g. `(call_expression function: (identifier) @fn (#eq? @fn "malloc"))`); optional `path_glob` and `capture` (filter to a single capture name). `SearchAstTool` lives in [src/tools/search_tools.h/cpp](src/tools/search_tools.h) alongside the other per-mode classes; not registered as a top-level tool — `SearchTool::execute` delegates on `mode=ast`. Each call constructs its own short-lived `TreeSitterRegistry` (cheap, sidesteps locking against the indexer's parser). Tree-sitter does NOT auto-enforce `#eq?` / `#match?` / `#any-of?` predicates — `match_passes_predicates()` walks `ts_query_predicates_for_pattern` steps and applies `eq?`/`not-eq?`/`match?`/`not-match?`/`any-of?`/`not-any-of?` against capture text. Output: `path:line  @capture  inline_text` plus a 2-line snippet, with `[truncated at max_results]` when capped. Curated reference query library under [queries/](queries/) (cpp / python / typescript / rust / go) — copy-paste starters for the LLM, not auto-loaded. Tool catalog unchanged (13). 13 new unit tests in [tests/test_search_ast.cpp](tests/test_search_ast.cpp) tagged `[s4.m][search][ast]`. The "rewrite `get_file_outline` on `.scm` queries" task from the stage spec was deferred — the rule-based extractors just landed via S3.D and a parallel `.scm` rewrite would have no user-visible effect.

**S4.J (Embeddings + Reranker) done.** Default embedder swapped from `all-MiniLM-L6-v2` (384-dim, English) to `bge-m3` (1024-dim, multilingual, 8K-capable). Embedding dimension is read from the GGUF (`llama_model_n_embd`) rather than configured — `WorkspaceConfig::embedding_dimensions` removed. New `Database::ensure_vectors_schema(dim)` + `meta` table in `vectors.db` detect dim mismatch on workspace open, drop `chunks` + `chunk_vectors`, and let the indexer re-chunk + re-enqueue every file (full re-embed). New [src/index/reranker.h/cpp](src/index/reranker.h) wraps llama.cpp with `LLAMA_POOLING_TYPE_RANK` for `bge-reranker-v2-m3`; loaded by `Workspace` when `reranker_enabled` is true and exposed through `IWorkspaceServices::reranker()`. `search_semantic` and `search_hybrid` now pull `reranker_top_k` candidates (default **20** — see stage doc for the perf reality) and re-score them with the cross-encoder before trimming to `max_results`. Reranker is **off by default** (opt-in via Settings → Index, or `config.json`). New [models/download.ps1](models/download.ps1) + [models/download-small.ps1](models/download-small.ps1) fetch the GGUFs (see [README.md](README.md#models)). Original "<200ms for 50 candidates" target was off by ~25× on bge-reranker-v2-m3 (568M params); the test now guards against catastrophic regressions only — see [roadmap/M4/S4.J-embeddings-reranker.md](roadmap/M4/S4.J-embeddings-reranker.md) for the full numbers.

**S4.N (Tool-Call Robustness Across Model Families) done.** New `ToolFormat` enum on `LLMConfig` (`Auto` / `OpenAi` / `Qwen` / `Claude` / `None`) plus a per-workspace `llm.tool_format` string in `.locus/config.json` (parsed via `tool_format_from_string`, unknown values fall back to Auto). `LMStudioClient` now holds a `std::unique_ptr<IStreamDecoder>` chosen by `make_decoder_for(ToolFormat)` instead of a hard-wired `OpenAiDecoder`; the four extra decoders all wrap an `OpenAiDecoder` for SSE chunk shape and add a new shared filter [src/llm/xml_tool_call_extractor.h/cpp](src/llm/xml_tool_call_extractor.h) that scans the visible-text channel for `<tool_call>{json}</tool_call>` (Qwen) or `<function_calls><invoke name="..."><parameter name="...">...</parameter></invoke></function_calls>` (Claude), with chunk-boundary-safe partial-marker hold-back. Native JSON `tool_calls` deltas pass straight through unchanged regardless of which dialect is selected -- the XML extraction is purely additive, so LM Studio's existing template translation never fights with the decoder. `Auto` (the default) watches both XML markers; `Qwen` / `Claude` lock to one each; `None` skips the `tools` array entirely (for models without tool training). New `IStreamDecoder::finish_stream(sink)` hook lets stateful decoders flush partial-tag bytes when the transport returns. `SystemPromptBuilder::build` gained an optional `ToolFormat` parameter that injects an ~80-token format-specific addendum for Qwen / Claude / None (Auto and OpenAi inject nothing -- LM Studio's chat template is doing the work). 24 new unit tests in [tests/test_stream_decoders.cpp](tests/test_stream_decoders.cpp) tagged `[s4.n]`. Live qwen3.5-9b @ 16k integration validates the OpenAI-passthrough path is unchanged; XML extraction is exercised by the unit tests since LM Studio's qwen template still translates `<tool_call>` back into JSON deltas before we see them.

**S4.D (Plan Mode) done.** *Phase 1 -- engine:* new `AgentMode` enum (`chat` / `plan` / `execute`) on `AgentCore`, mutated via queued requests on the agent thread (same pattern as `compact_context` / S3.I). Two new tools in [src/tools/plan_tools.h/cpp](src/tools/plan_tools.h): `propose_plan` (visible only in plan mode, validates `steps[].description`, returns `{"ok":true,...}` JSON) and `mark_step_done` (visible only in execute mode, validates `step` index + `status` enum). `ToolMode::execute` added; default `visible_in_mode` updated so existing tools are visible in agent + execute (full catalog) and hidden from plan + subagent. `AgentLoop::run_step(history, ToolMode)` takes the mode per call so the API tool catalog filters per round. New `IFrontend` callbacks: `on_mode_changed`, `on_plan_proposed`, `on_plan_step_advanced`, `on_plan_completed`; `ILocusCore` gains `set_mode`/`mode`/`approve_plan`/`reject_plan`. After each tool dispatch, `AgentCore::observe_plan_tool_result` parses the result JSON and updates plan state -- propose_plan sets `plan_awaiting_decision_` and breaks the round loop so the agent waits for the user; approve_plan switches mode to execute, builds a structured plan-recap message, and pushes it onto the queue so the agent resumes automatically; reject_plan clears the plan and stays in plan mode. No system-prompt mode addendum -- the tool descriptions + `visible_in_mode` filtering carry the mode behavior, so the system prompt stays byte-stable across mode switches (S4.F-friendly). *Phase 2 -- GUI:* three-state `Chat | Plan | Execute` switcher (`wxToggleButton` row above the chat input); plan bubble in chat (`addPlanMsg(id, json)` JS helper renders a numbered list with status glyphs `○ ⧗ ✓ ✗`, Approve / Reject buttons routed via `locus://plan-approve|reject/<id>` URLs caught in `wxEVT_WEBVIEW_NAVIGATING`); footer chip next to the context meter (`Plan: 3/7 - building scene graph`); `EVT_AGENT_MODE_CHANGED` / `EVT_AGENT_PLAN_PROPOSED` / `EVT_AGENT_PLAN_STEP_ADVANCED` / `EVT_AGENT_PLAN_COMPLETED` thread-bridged through `WxFrontend`. 21 unit tests `[s4.d]` + 3 integration tests `[plan]` against gemma-4-e4b. See [roadmap/M4/S4.D-plan-mode.md](roadmap/M4/S4.D-plan-mode.md).

**S4.G (MCP Client) done.** *Phase 2 -- engine debt + Settings UI:* `IToolRegistry::unregister_tool` removes the hot-restart skip-on-duplicate workaround so an MCP server that restarts with a different tool set reflects the new set exactly. `StdioTransport::reader_loop` waits up to 2 s for the process handle to signal after the pipe closes, calls `GetExitCodeProcess`, and surfaces `(exit_code, has_exit_code)` through `McpClient` and `McpManager::ServerStatus`; the Settings UI shows the code next to the status label so the user can distinguish a graceful exit from a crash. `McpTool::execute` caps each result body at 1 MB (matching `CheckpointStore`'s per-file cap) -- oversized results get a `[truncated N bytes ...]` suffix and a warn log so a buggy server can't poison `ConversationHistory` before the context-budget soft trigger fires. `ToolDispatcher`'s approval resolution pulled into `tools::resolve_approval_policy` (exact match wins; `mcp:<server>:*` is a per-server trust prefix; bare `*` is deliberately unsupported), enabling a one-click **Trust** toggle in the new Settings MCP tab that writes the prefix entry into `tool_approval_policies` without touching individual tools. `SettingsDialog` refactored to a `wxNotebook` with four tabs (LLM / Index / Tool Approvals / **MCP Servers**); the MCP tab lists every configured server with `name | status | tool count | command`, has a Restart button (`McpManager::restart`), an Open mcp.json button (creates a stub when missing and shells out to the default editor), a per-server Trust checkbox, and a detail box that shows the status label + exit code + namespaced tool list + last error. `LocusFrame` ctor accepts an `McpManager*` so the dialog can pick it up via `LocusSession::mcp()`; declaration order in `LocusSession` swapped so `mcp_` destroys before `tools_` (the previous order UAF'd in `~McpManager -> stop_all -> registry_.unregister_tool`, caught at session teardown by the `[mcp]` integration test as a SIGSEGV). `LocusApp::OnInitCmdLine` / `OnCmdLineParsed` overrides declare the workspace positional + `--endpoint`/`--model`/`--context` so the wxApp default parser doesn't reject them and silently kill startup. New [architecture/mcp.md](architecture/mcp.md) documents the full `mcp.json` schema, merge precedence, namespacing, approval/trust paths, lifecycle state machine, and the result cap. Phase 2 added 4 unit tests (`[s4.g][tools]` / `[s4.g][tools][approval]` / `[s4.g][mcp][tool]` / `[s4.g][mcp][manager]` stop_all-unregister) on top of the phase-1 13 + the `[integration][llm][mcp]` end-to-end case. Real-server validation against `@modelcontextprotocol/server-filesystem` remains the recommended user-side smoke before declaring S4.G fully shipped. See [roadmap/M4/S4.G-mcp.md](roadmap/M4/S4.G-mcp.md).

*Phase 1 -- engine:* new `src/mcp/` layer plugs Model Context Protocol stdio servers into Locus's tool catalog. Six pairs in `locus_core`: [src/mcp/mcp_types.h](src/mcp/mcp_types.h) (`McpServerConfig`, `McpToolDefinition`); [src/mcp/mcp_config.h/cpp](src/mcp/mcp_config.h) loads `.locus/mcp.json` + global `%APPDATA%/Locus/mcp.json` and merges them (workspace overrides global by server name); [src/mcp/json_rpc.h/cpp](src/mcp/json_rpc.h) is a ~100-LOC JSON-RPC 2.0 helper + tolerant parser; [src/mcp/stdio_transport.h/cpp](src/mcp/stdio_transport.h) spawns child processes under a Win32 Job Object (`KILL_ON_JOB_CLOSE`) with merged-env block, line-oriented reader thread, and CancelIoEx-based shutdown; [src/mcp/mcp_client.h/cpp](src/mcp/mcp_client.h) does the `initialize` handshake, wraps `tools/list` + `tools/call` in synchronous promise/future request/response over the transport, and runs a `not_started -> initializing -> ready -> crashed/stopped` state machine; [src/mcp/mcp_tool.h/cpp](src/mcp/mcp_tool.h) is the `ITool` shim using namespaced names `mcp:<server>:<tool>` and forwarding the server's `inputSchema` verbatim; [src/mcp/mcp_manager.h/cpp](src/mcp/mcp_manager.h) owns the per-workspace pool, drains every enabled server's `tools/list` at startup, and registers the proxies into `IToolRegistry`. New optional virtual `ITool::parameters_schema()` lets MCP tools ship the full JSON Schema (nested objects, enums, `oneOf`) without lossy round-tripping through the flat `ToolParam` list -- built-in tools leave the default empty implementation and the registry derives the schema from `params()` as before. Wired into `LocusSession` between `tools_` and `agent_` (declaration order ensures `mcp_` destroys before `tools_`, so `McpTool::client_` borrows aren't held past the `McpClient`'s lifetime; in practice McpTool dtors don't dereference, so the rule is belt + braces). Approval defaults to `ask` (MCP tools untrusted until users opt in via the existing per-tool `tool_approvals` map keyed by `mcp:<server>:<tool>` -- no new code path in the dispatcher). Server crash flips `McpTool::available()` to false so the LLM stops seeing the tools without unregistering them; `on_crash` callback emits a status event for the GUI (phase 2). Bundled mock server: tiny [tests/mock_mcp_server/main.cpp](tests/mock_mcp_server/main.cpp) implements `initialize` + `tools/list` + `tools/call` with `echo` and `fail_once` tools, built as a `locus_mock_mcp_server` exe placed next to `locus_tests.exe` (resolved via `GetModuleFileName`); `LOCUS_MOCK_MCP_CRASH_AFTER=<n>` env triggers crash simulation. 13 unit tests in [tests/test_mcp.cpp](tests/test_mcp.cpp) tagged `[s4.g][mcp]` cover JSON-RPC parsing, config merge, stdio handshake, tools/list + tools/call round trip, `isError` propagation, crash lifecycle, manager registration, and graceful no-op when a configured command can't be spawned. End-to-end coverage lives in [tests/integration/test_int_mcp.cpp](tests/integration/test_int_mcp.cpp) (`[integration][llm][mcp]`): live LLM picks `mcp:mock:echo` on a self-contained `LocusSession`, full round-trip is asserted via the captured tool call + result; soft-skips when the small model fails to pick the MCP tool. Tool catalog rises from 13 built-ins to 13 + N MCP tools per workspace. Phase 2 (Settings UI panel + per-server "trust" toggle) and Streamable-HTTP transport are deferred. See [roadmap/M4/S4.G-mcp.md](roadmap/M4/S4.G-mcp.md).

**S4.R (Memory Bank -- Persistent + Searchable) Phase 1 done.** Workspace-scoped persistent memory: each entry lives in `.locus/memory/<id>.md` as a verbatim markdown file with YAML frontmatter (id / created_at / updated_at / last_used_at / source / pinned / tags); `.deleted/` holds soft-deleted entries for 30-day retention. New [src/core/memory_store.h/cpp](src/core/memory_store.h) `MemoryStore` owns the directory, prepares per-statement handles against a new `memory_fts` FTS5 virtual table in `index.db` and a `memory_vectors` vec0 table in `vectors.db` (keyed by an autoincrementing rowid in a `memory_keys(rowid, id)` mapping table, since vec0's primary key is integer). On dim/model wipe `Database::ensure_vectors_schema` drops `memory_vectors` + clears `memory_keys` alongside `chunk_vectors`, so a workspace embedder swap forces a memory re-embed automatically; on construction the store reconciles FTS5 + vector rows against the on-disk `.md` files (re-embeds anything missing). `Workspace` owns the store and exposes it via `IWorkspaceServices::memory()`. Two new tools in [src/tools/memory_tools.h/cpp](src/tools/memory_tools.h) -- `add_memory` (default approval `ask`, durable mutation) and `search_memory` (auto-approve; FTS5 BM25 + cosine + soft 21-day recency factor + optional reranker pass gated on the workspace's existing `reranker_enabled` flag, no separate per-corpus knob). Both gate themselves with `available(ws) = ws.memory() != nullptr` so a memory-disabled workspace drops them from the per-turn manifest. `/memorize [+tag1 +tag2] [--pin] <content>` slash command lives in `AgentCore::try_slash_command`; parses tag tokens and the pin flag, writes through the store, emits a `memory_added` activity event with the entry id + tag preview (new `ActivityKind::memory_added` / `memory_searched` / `memory_deleted`). Companion `/forget <id> [--hard]` slash command for user-side deletion: soft by default (moves to `.deleted/` for 30-day retention), `--hard` skips the trash and removes permanently. No model-side delete tool by design — the agent grows memory, the user prunes it. `SystemPromptBuilder::build` grew an optional pre-rendered `memory_section` parameter -- AgentCore ctor pulls it from `MemoryStore::format_for_system_prompt()` which composes "## Memory Bank" from all pinned entries (subject to budget) followed by most-recently-used unpinned until `memory.in_context_budget_tokens` (default 500) is exhausted; section is captured once at session start so the base prompt stays byte-stable across rounds (KV-cache-friendly for the future S4.F work). Empty section = suppressed entirely (no header noise on first session). Entries added mid-session via `/memorize` or `add_memory` are searchable immediately but don't auto-inject into the current session's prompt; they pick up at next session start. `WorkspaceConfig` gained a `memory` block (`enabled` / `in_context_budget_tokens` / `max_entries` / `search_response_max_tokens` / `recency_half_life_days`) under `"memory":{}` in `.locus/config.json`; the `enabled` flag is the eventual S5.A capability-bucket hook. 16 unit tests / 61 assertions in [tests/test_memory_store.cpp](tests/test_memory_store.cpp) tagged `[s4.r][memory-store]`. Tool catalog rises from 15 to 17. Phase 2 (Settings -> Memory Bank tab + chat right-click "Save as memory" + tool-result observer for tool-path `memory_*` activity events) deferred. See [roadmap/M4/S4.R-memory-bank.md](roadmap/M4/S4.R-memory-bank.md).

**S4.U (Stream Decode Completeness + max_tokens Discipline) done.** Closes a robustness gap S4.N didn't cover: with qwen3.6-35b-a3b in LM Studio, a multi-file `write_file` request hit the hardcoded `max_tokens=2048` cap mid-stream, leaving the second tool call with `arguments=""`. The malformed call landed in `ConversationHistory` and the next round to LM Studio failed with HTTP 500 because the chat template can't render an empty arguments string. Three fixes, three layers: (1) `WorkspaceConfig::llm_max_tokens` (default `8192`, was hardcoded `2048`) plumbed through `LocusSession::merge_workspace_defaults` and exposed in Settings → LLM. (2) `StreamDecoderSink` gained `on_finish_reason` and `on_stream_error`; `OpenAiDecoder` now decodes `choices[0].finish_reason` (`stop` / `length` / `tool_calls` / `content_filter`), `delta.refusal` (routed through `on_text` with a `[refusal]` prefix), top-level `{"error":...}` chunks, and `usage.prompt_tokens_details.cached_tokens` (new `CompletionUsage::cached_tokens` field, ready for telemetry). All four decoders forward the new sink slots. (3) `LMStudioClient::stream_completion` warns + fires `on_error` when `finish_reason == "length"` (and on any non-trivial stop reason like `content_filter`), then validates each accumulated tool call's `arguments` parses as JSON before delivery -- empty / malformed entries are dropped with a clear error message that names truncation as the cause when applicable. The XML decoders' existing in-extractor body validation already covered them; the new gate is primarily an OpenAI-path safety net. 8 new `[s4.u]` unit tests in [tests/test_stream_decoders.cpp](tests/test_stream_decoders.cpp). See [roadmap/M4/S4.U-stream-decode-completeness.md](roadmap/M4/S4.U-stream-decode-completeness.md).

**S4.L (Git Integration -- .gitignore + Auto-Commit) done.** Two independently-toggleable behaviours behind one stage; both live in `WorkspaceConfig` (`index.respect_gitignore` default true, `git.{auto_commit,commit_branch,commit_prefix}` defaults `false / "" / "[locus] "`). *gitignore respect:* new [src/index/gitignore.h/cpp](src/index/gitignore.h) parses root + nested `.gitignore` files (skipping `{.git, .locus, node_modules, build, dist, out, target}` so vendored sub-repos under `build/` don't pull thousands of patterns), normalises entries into glob form, and uses a small recursive matcher with proper `**` backtracking (the simpler [src/index/glob_match.h](src/index/glob_match.h) used for fixed `exclude_patterns` doesn't backtrack on inner `*` failures). `Indexer` holds the merged pattern list; `is_excluded()` checks both sets; new `reload_gitignore()` + `reconcile_excluded_files()` handle live `.gitignore` edits (detected as filename match in `process_events` and processed before the rest of the batch) and on-open cleanup of stale rows from prior sessions. Negation patterns (`!foo`) are dropped per spec. *Auto-commit:* new [src/agent/auto_commit.h/cpp](src/agent/auto_commit.h) free function runs a small Win32 sync subprocess pipeline (separate from S4.I `RunCommandTool` -- no Job Object / timeout overhead for short-lived `git ...` calls): detect `.git/` -> optional branch switch (creating + warning if missing) -> `git add -A` -> `git commit -F <tempfile>` (avoids cmd.exe quoting headaches) -> `git rev-parse --short HEAD`. "Nothing to commit" stderr is treated as `skipped=true`, not failure. `AgentCore::process_message` calls it after the round loop when (a) workspace config has `git_auto_commit=true`, (b) `change_tracker_->agent_touched_size() > 0`, and (c) the turn didn't end on an error. New `IFrontend::on_auto_commit(short_sha, branch, subject)` callback flows through `WxFrontend` -> `EVT_AGENT_AUTO_COMMIT` -> `LocusFrame` -> `ChatPanel`'s new `commit_chip_` in the footer (hidden until the first commit lands). Tool catalog unchanged. 11 unit tests in [tests/test_gitignore.cpp](tests/test_gitignore.cpp) tagged `[s4.l][gitignore]` + 9 in [tests/test_auto_commit.cpp](tests/test_auto_commit.cpp) tagged `[s4.l][git][autocommit]` (skip with `SUCCEED` when `git` isn't on PATH). Manual GUI test plans at [tests/manual/gitignore.md](tests/manual/gitignore.md) and [tests/manual/auto-commit.md](tests/manual/auto-commit.md).

**S4.F (KV / Prompt Cache Preservation) done.** Keeps LM Studio / llama.cpp prefix caching alive by enforcing a byte-stable system message across a session, plus exposes the cache-hit signals so future regressions are obvious. *System prompt invariant:* the attached-file outline used to be appended to `base_system_prompt_` and re-seeded into history via `replace_system_prompt()` on every `set_attached_context()` / `clear_attached_context()` -- each attach/detach torched the cache from message 0 onward. `AgentCore::compose_attached_context_block()` now composes the same block (`[Attached file: <path>] / Outline: / Preview:`) per-user-turn and prepends it to `effective_content` alongside S4.T's `[Files changed since last turn: ...]` note; the system message becomes byte-stable for the life of a session. The corresponding `refresh_system_prompt()` / `compose_system_prompt()` pair was deleted (now `system_prompt_ = base_system_prompt_` everywhere). Invariant documented at the top of [src/agent/system_prompt.h](src/agent/system_prompt.h) so future stages don't regress it. *Per-round diagnostic log:* `AgentLoop::run_step` computes `std::hash` over `messages[0].content` before every POST and logs one info line at end of round -- `LLM POST: sys_hash=<hex> sys_chars=<n> prompt=<n> cached=<n> completion=<n> ttft_ms=<n> prefill_ms_per_prompt_token=<rate>`. Quoting two adjacent rounds answers "did the cache fire" without needing LM Studio's raw-prompt log. *MetricsAggregator extensions:* `TurnSample` gained `cached_tokens` (sum across rounds), `prompt_prefix_hash` (first-round-wins per turn), `ttft_ms` (first-round-wins per turn); new `record_prompt_prefix_hash(h)` / `record_first_token(ms)` slots called from `AgentLoop`. `Aggregates` now carries `cached_tokens_total`, `cached_token_ratio`, `prompt_prefix_stable` (true iff the last two turns share the same hash), `last_ttft_ms`, `last_prefill_ms_per_token`. The `/metrics` text now includes `kv-cache: cached=<N> ratio=<%> prefix_stable=yes|no` and `prefill: ttft=<ms> ms/prompt_tok=<rate>` lines; same rows appear in the GUI `MetricsView`. JSON/CSV export carry the new fields too. *Generation-progress callback:* new `IFrontend::on_generation_progress(int chars, int est_tokens)` default no-op; `AgentLoop` accumulates streamed text+reasoning bytes and fires the callback throttled to 150 ms via `chrono::steady_clock` (final force-fire on stream end). GUI renders a `GEN ~N,NNN tok` chip in the chat footer between the auto-commit chip and the LOCUS.md chip; hides on `on_turn_complete` since the context meter then carries the exact `completion_tokens`. *Field validation, 2026-05-13:* live-tested with gemma-4-e4b on LM Studio. Two consecutive turns produced `sys_hash=651757b3cd226322` both times (invariant holds); TTFT collapsed from 4911 ms -> 302 ms across the same-prefix turns -- an ~18x prefill speedup despite LM Studio reporting `cached_tokens=0` (as the spec predicted, the LM Studio build doesn't populate `prompt_tokens_details.cached_tokens` even when caching fires; TTFT is the ground-truth signal). 5 new `[s4.f][metrics]` unit tests in [tests/test_metrics.cpp](tests/test_metrics.cpp); 2 rewritten `[s2.4][s4.f]` cases in [tests/test_attached_context.cpp](tests/test_attached_context.cpp) asserting the system message is byte-stable across attach/detach; one new `[integration][llm][metrics][s4.f]` case in [tests/integration/test_int_metrics.cpp](tests/integration/test_int_metrics.cpp). See [roadmap/M4/S4.F-kv-cache.md](roadmap/M4/S4.F-kv-cache.md).

**S4.W (list_directory: surface unindexed files) done.** Closes the "is there a logo in `assets/`?" footgun -- `ListDirectoryTool` used to go straight through `IndexQuery::list_directory`, so anything the indexer skipped (binaries without extractors, oversized files, unknown extensions) was invisible. The tool now enumerates the real filesystem via `std::filesystem::directory_iterator` (manual recursion respecting the `depth` arg, no symlink following) and uses the index purely for *annotation*: one `IndexQuery::list_directory(path, huge_depth)` call builds a `path -> {size, language, is_binary}` hashmap, then each filesystem entry merges in its indexed metadata. Per-entry annotation: `[<lang>]` for indexed text, `[binary]` for files matching a header-internal binary-extension list (PNG/JPG/PDF/ZIP/EXE/DLL/MP3/MP4/...) or indexer-flagged binaries, `[oversized]` when on-disk size exceeds `WorkspaceConfig::max_file_size_kb*1024` (per the stage doc, derived at tool time -- no schema change), `[unindexed]` for everything else (model is expected to try `read_file` if it suspects text). Workspace `exclude_patterns` are still enforced at the tool layer (intentional policy) -- directory entries get a `rel + "/x"` probe so patterns like `.git/**` match the directory itself, not just contents. Output capped at `max_entries` (new optional tool param; default 200) with `[N more entries truncated; refine path or use search]` footer. `IndexQuery::list_directory` contract unchanged -- it's still indexed-only and shared with regex search + file-change tracker. 7 new `[s4.w][list_directory]` Catch2 cases in [tests/test_list_directory_tool.cpp](tests/test_list_directory_tool.cpp). See [roadmap/M4/S4.W-list-directory-completeness.md](roadmap/M4/S4.W-list-directory-completeness.md).

See [roadmap/M4/](roadmap/M4/) for remaining stages.

**M3 is now Refactoring** (not Agent Quality). The renumbering trail: original M3 → M4 (Agent Quality), original M4 → was M5, now M6 (Connected). A new M5 (Polish, UX & Performance) was inserted between M4 and the old Connected milestone. Per-stage docs live under [roadmap/M3/](roadmap/M3/), [roadmap/M4/](roadmap/M4/), [roadmap/M5/](roadmap/M5/), [roadmap/M6/](roadmap/M6/). [roadmap.md](roadmap.md) is the index.

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
```

| Target | Output (Release) |
|---|---|
| `locus` (CLI)         | `build/release/Release/locus.exe` |
| `locus_gui`           | `build/release/Release/locus_gui.exe` |
| `locus_tests`         | `build/release/tests/Release/locus_tests.exe` |
| `locus_integration_tests` | `build/release/tests/integration/Release/locus_integration_tests.exe` |
| `locus_retrieval_eval`    | `build/release/tests/retrieval_eval/Release/locus_retrieval_eval.exe` |

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
# Always pass -console when running on user request -- it pops a dedicated trace
# console window. Without it stderr stays at warning-level only.
build/release/tests/integration/Release/locus_integration_tests.exe -console

# One tag area
build/release/tests/integration/Release/locus_integration_tests.exe "[smoke]" -console
build/release/tests/integration/Release/locus_integration_tests.exe "[search]" -console
# (also: [outline] [fs] [shell] [bg] [ask_user] [slash] [file_change_awareness] [undo] [metrics] [max_tokens] [plan] [mcp])
```

When you launch with `-console` from Bash/subprocess, captured stdout looks empty
(stdout is redirected into the new window). For pass/fail rely on the exit code;
for post-mortem detail point the user at `<workspace>/.locus/integration_test.log`.

### Retrieval eval (manual)

```
build/release/tests/retrieval_eval/Release/locus_retrieval_eval.exe \
    --workspace . \
    --queries tests/retrieval_eval/queries.json \
    --out tests/retrieval_eval/results.md
```

See [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md) for baseline
management and the regression-check protocol.

---

## Code Map

Source is in `src/`, tests in `tests/`, architecture docs in `architecture/`.
Core is a static lib (`locus_core`). Both `locus` (exe) and `locus_tests` link it.

| File | What it owns | Key types |
|---|---|---|
| `src/core/locus_session.h/cpp` | Single-ctor/dtor bundle of Workspace + ILLMClient + ToolRegistry + AgentCore (S3.G). Ctor handles wiring order + LLM-config layering (defaults → `.locus/config.json` → caller seed → live `query_model_info`); dtor stops the agent thread first, then unwinds in reverse declaration order. Both CLI (`main.cpp`) and GUI (`LocusApp`) construct one of these per open workspace; future lifecycle subsystems (LSP, MCP) plug in here. | `LocusSession` |
| `src/core/workspace.h/cpp` | Opens a folder, owns main_db (always) + vectors_db (optional) + watcher + indexer + query + watcher_pump. One per folder. | `Workspace`, `WorkspaceConfig` |
| `src/core/database.h/cpp` | RAII SQLite wrapper. WAL mode. Two schema kinds: `Main` (files/fts/symbols/headings) and `Vectors` (chunks + `meta` + vec0 chunk_vectors; loads sqlite-vec only here). `chunk_vectors` is created lazily by `ensure_vectors_schema(dim)`, which also performs the dim-mismatch wipe + re-embed migration. Includes legacy-table migration from the pre-S3 split. | `Database`, `DbKind` |
| `src/core/file_watcher.h/cpp` | efsw wrapper. Debounced `FileEvent` queue. | `FileWatcher`, `FileEvent` |
| `src/index/indexer.h/cpp` | Initial traversal + incremental updates. Owns `IndexerStatements` + `TreeSitterRegistry` + `SymbolExtractorRegistry`; uses `ExtractorRegistry` for text/heading extraction. Writes skeleton to main_db, chunks to vectors_db — transactions open on both connections in parallel. After S3.D, `index_file` is purely orchestration: read → extract → upsert → symbols → chunk → persist. | `Indexer`, `Indexer::Stats` |
| `src/index/index_query.h/cpp` | Read-only queries: FTS5 search, symbol lookup, outlines, dir listing. Semantic search is a two-step read — vectors_db for top-K chunks, main_db to resolve file_id → path. | `IndexQuery`, `SearchResult`, `SymbolResult`, `OutlineEntry`, `FileEntry` |
| `src/index/tree_sitter_registry.h/cpp` | Owns the `TSParser*` and the `name → TSLanguage*` map (9 grammars). `Indexer` holds one as a member; designed to be shared with S4.M `ast_search`. Caller must serialise parser use. | `TreeSitterRegistry` |
| `src/index/symbol_extractor.h/cpp` | `ISymbolExtractor` interface returning `vector<ExtractedSymbol>`, plus a `SymbolExtractorRegistry` keyed by language name. The shared two-level walk + `extract_name_from_node` helper live on a private `RuleBasedSymbolExtractor` inside the .cpp. `register_builtin_symbol_extractors()` wires the seven languages into the supplied registry. | `ISymbolExtractor`, `ExtractedSymbol`, `SymbolRule`, `SymbolExtractorRegistry` |
| `src/index/symbol_extractors/<lang>_extractor.cpp` | One file per language (cpp / python / js_ts / go / rust / java / csharp). Each holds only its `SymbolRule` table and a `make_<lang>_symbol_extractor()` factory that forwards to `make_rule_based_symbol_extractor`. No DB or Tree-sitter walking code. | — |
| `src/index/prepared_statements.h/cpp` | RAII holder for the 14 indexer prepared statements (10 main.db + 4 vectors.db; the vectors-db ones stay null when semantic search is disabled). ctor prepares everything, dtor finalises. Members are public so `Indexer` accesses them as `stmts_.upsert_file` etc. | `IndexerStatements` |
| `src/index/chunker.h/cpp` | Content chunking: code (Tree-sitter boundaries), document (heading boundaries), sliding window fallback. | `Chunk`, `SymbolSpan`, `chunk_code()`, `chunk_document()`, `chunk_sliding_window()` |
| `src/index/glob_match.h` | Minimal glob matcher (`*`, `**`, `?`) used by the indexer's exclude patterns and the regex-search tool. Header-only. | `glob_match()` |
| `src/index/gitignore.h/cpp` | S4.L `.gitignore` parser + matcher. `parse_gitignore_text` normalises one file's lines into glob patterns scoped to the file's directory; `load_workspace_gitignore` walks the workspace (skipping `{.git, .locus, node_modules, build, dist, out, target}` to avoid pulling thousands of patterns from vendored sub-repos under `build/`); `gitignore_match` is a small recursive matcher with proper `**` backtracking (the simpler `glob_match` doesn't backtrack on inner `*` failures). Negation patterns (`!foo`) are dropped per S4.L spec. | `GitignorePattern`, `parse_gitignore_text`, `load_workspace_gitignore`, `gitignore_match` |
| `src/llm/llm_client.h/cpp` | Public `ILLMClient` interface + types (`ChatMessage`, `ToolCallRequest`, `LLMConfig`, `StreamCallbacks`, `CompletionUsage`, `ToolSchema`, `ModelInfo`, `ToolFormat`). The .cpp holds the concrete `LMStudioClient` (composes `OpenAiTransport` + a per-`ToolFormat` `IStreamDecoder`), the `create_llm_client` factory, and the `make_decoder_for` helper. Post-S3.B: no `estimate_tokens` static -- see `TokenCounter`. | `ILLMClient`, `LMStudioClient`, `ChatMessage`, `LLMConfig`, `StreamCallbacks`, `ToolFormat` |
| `src/llm/openai_transport.h/cpp` | cpr POST `/v1/chat/completions` + SSE feed + one-shot stall retry + HTTP error reporting. Hands each `data:` payload to its callback as a raw string and swallows `[DONE]`. Knows nothing about JSON. | `OpenAiTransport` |
| `src/llm/sse_parser.h/cpp` | Low-level SSE `data:` line parser. Used by `OpenAiTransport`. | `SseParser` |
| `src/llm/stream_decoder.h` | `IStreamDecoder` interface -- translates one SSE payload into typed events (`on_text` / `on_reasoning` / `on_tool_call_delta` / `on_usage` / `on_finish_reason` / `on_stream_error`) via a stack-allocated `StreamDecoderSink`. Decoders may keep per-stream state (XML formats need it). Three optional hooks: `decode()` per chunk, `finish_stream(sink)` once at end-of-stream to flush held-back partial-tag bytes (S4.N), and `reset()` between streams. `on_finish_reason` and `on_stream_error` (S4.U) carry `length` / `content_filter` / refusal / mid-stream `{"error":...}` events the LMStudioClient turns into user-visible warnings. | `IStreamDecoder`, `StreamDecoderSink` |
| `src/llm/xml_tool_call_extractor.h/cpp` | S4.N -- streaming filter that scans a text channel for embedded XML tool calls (`<tool_call>{json}</tool_call>` Qwen, `<function_calls><invoke...>...</invoke></function_calls>` Claude) and extracts them as `ToolCallDelta` events. Holds back partial-marker suffixes across chunks; truncated bodies surface as text on `finish()`. Shared by Qwen / Claude / Auto decoders below. | `XmlToolCallExtractor`, `XmlMarker` |
| `src/llm/stream_decoders/openai_decoder.h/cpp` | Default decoder for OpenAI-compatible chunks (`choices[0].delta.{content,reasoning_content,tool_calls}` + top-level `usage`). Stateless across calls. | `OpenAiDecoder` |
| `src/llm/stream_decoders/qwen_xml_decoder.h/cpp` | S4.N -- composes `OpenAiDecoder` (chunk shape) with `XmlToolCallExtractor{Qwen}` (text channel). Native JSON `tool_calls` deltas pass through; embedded `<tool_call>` markers are extracted with a synthetic `call_xml_<n>` id and JSON-parsed body. | `QwenXmlDecoder` |
| `src/llm/stream_decoders/claude_xml_decoder.h/cpp` | S4.N -- same shape as Qwen but for `<function_calls><invoke...>` blocks; multiple invokes per block emit one `ToolCallDelta` each. Parameter values are run through a JSON-parse-with-string-fallback so numbers / booleans / arrays land as their natural types. Decodes the five common XML entities. | `ClaudeXmlDecoder` |
| `src/llm/stream_decoders/auto_decoder.h/cpp` | S4.N default. Watches both Qwen and Claude markers simultaneously; the OpenAI native path remains untouched, so this is the right pick when the user hasn't pinned a `tool_format` for their model. | `AutoToolFormatDecoder` |
| `src/llm/llm_router.h/cpp` | S4.Q hook. Owns a strong + optional weak `ILLMClient`; `select(Role)` returns the right one (today: weak falls back to strong). Not yet wired into `LocusSession`. | `LlmRouter` |
| `src/llm/token_counter.h/cpp` | Heuristic token estimator (~4 chars = 1 token, +4 framing per message). Lifted off `ILLMClient` in S3.B so callers (`AgentCore`, `AgentLoop`, `ConversationHistory`, `SystemPromptBuilder`, `CompactionDialog`, tests) don't drag the LLM client interface in just to count tokens. S4.F will introduce real BPE tokenisation behind this same surface. | `TokenCounter` |
| `src/core/workspace_services.h` | `IWorkspaceServices` interface — tool-facing surface over a workspace (no .cpp — pure abstract). `Workspace` implements it; tests use `tests/support/fake_workspace_services.h`. | `IWorkspaceServices` |
| `src/core/watcher_pump.h/cpp` | Background thread that drains FileWatcher, batches by quiet-period (1.5s) + hard-cap (20s), calls `Indexer::process_events`. Owned by `Workspace` — replaces the pump that used to live duplicated in main.cpp / locus_app.cpp / locus_frame.cpp. Provides `flush_now()` for synchronous external flush. | `WatcherPump` |
| `src/core/workspace_lock.h/cpp` | RAII exclusive file lock on `.locus/locus.lock`. Acquired first in `Workspace` ctor, released last in dtor. Refuses to open when the same workspace — or any ancestor workspace — is already held by another Locus process. OS releases the lock on crash. Replaces the process-global `wxSingleInstanceChecker` in the GUI so two different workspaces can run side by side. | `WorkspaceLock` |
| `src/core/memory_store.h/cpp` | S4.R workspace-scoped memory bank. Owns `.locus/memory/<id>.md` (verbatim entries with YAML frontmatter) + the `memory_fts` FTS5 table in main_db + the `memory_vectors` vec0 table in vectors_db (with a `memory_keys(rowid, id)` mapping so vec0's integer-keyed rows can reference text ids). Add / update / soft_delete (`.deleted/` retention) / hard_delete (skips retention, used by `/forget --hard` and the future Settings UI "delete permanently" button) / restore_deleted / purge_deleted (30-day retention) / gc (max-entries cap). Hybrid `search` blends BM25 + cosine + soft 21-day recency factor + optional reranker. `format_for_system_prompt()` composes the "## Memory Bank" slot for the system-prompt builder -- pinned first then LRU within `memory.in_context_budget_tokens`. Owned by `Workspace`, exposed via `IWorkspaceServices::memory()`. Single-writer / multi-reader (per-store mutex); reconciles FTS5 + vector rows against on-disk `.md` on construction so a deleted vectors.db or model swap re-embeds cleanly. | `MemoryStore`, `MemoryStore::Entry`, `MemoryStore::SearchHit` |
| `src/tools/tool.h` | Tool system interfaces (no .cpp — pure abstract + structs). | `ITool`, `IToolRegistry`, `ToolParam`, `ToolResult`, `ToolCall` |
| `src/tools/tool_registry.h/cpp` | Concrete registry. Schema JSON builder. ToolCall parser. | `ToolRegistry` |
| `src/tools/tools.h/cpp` | Aggregator header (re-exports every tool) + `register_builtin_tools()` factory. Individual tools live in the family headers below. | `register_builtin_tools` |
| `src/tools/shared.h/cpp` | `resolve_path` / `make_relative` / `error_result` helpers shared by tool implementations. Namespace `locus::tools`. | — |
| `src/tools/file_tools.h/cpp` | File CRUD + exact-string-diff edit tool (`EditFileTool` takes an `edits[]` array — single edit = array of one, batch = N items applied atomically). `WriteFileTool` subsumes the old `CreateFileTool` via an `overwrite` flag (default `false` refuses to replace existing files). | `ReadFileTool`, `WriteFileTool`, `EditFileTool`, `DeleteFileTool` |
| `src/tools/search_tools.h/cpp` | Unified `SearchTool` (mode: text/regex/symbols/semantic/hybrid) — the only search tool registered in the built-in manifest. The per-mode `Search*Tool` classes remain as internal implementations the dispatcher delegates to. | `SearchTool`, `SearchTextTool`, `SearchRegexTool`, `SearchSymbolsTool`, `SearchSemanticTool`, `SearchHybridTool` |
| `src/tools/index_tools.h/cpp` | Listing + outline tools. `ListDirectoryTool` (S4.W) enumerates the real filesystem via `std::filesystem::directory_iterator` and uses `IndexQuery::list_directory` only for annotation -- one hashmap lookup per filesystem entry merges in indexed `size_bytes` / `language` / `is_binary`. Per-entry annotation: `[<lang>]` for indexed text, `[binary]` for binary-extension files or indexer-flagged binaries, `[oversized]` for files past `WorkspaceConfig::max_file_size_kb`, `[unindexed]` for everything else (model is expected to try `read_file` if it suspects text). Workspace `exclude_patterns` are still enforced at the tool layer (`.git/**`, `node_modules/**`, `build/**`, `.locus/**`, etc.) so excluded dirs never surface; directory entries are checked via a `rel + "/x"` probe so patterns like `.git/**` match the directory itself, not just contents. Output is capped at `max_entries` (default 200, override via new tool param) with a `[N more entries truncated; refine path or use search]` footer. `IndexQuery::list_directory` contract unchanged (still indexed-only) -- the tool is the policy layer. | `ListDirectoryTool`, `GetFileOutlineTool` |
| `src/tools/process_tools.h/cpp` | Synchronous shell execution + S4.I background-process tools (`run_command_bg`, `read_process_output`, `stop_process`, `list_processes`). The bg tools mark `available()=false` when the workspace has no `ProcessRegistry`. | `RunCommandTool`, `RunCommandBgTool`, `ReadProcessOutputTool`, `StopProcessTool`, `ListProcessesTool` |
| `src/tools/process_registry.h/cpp` | Per-workspace registry of long-running shell processes (S4.I). Each child runs under a Win32 Job Object (`KILL_ON_JOB_CLOSE`) so grandchildren spawned via `cmd /c` are killed with the parent; one reader thread per process drains stdout+stderr into a ring buffer capped at `WorkspaceConfig::process_output_buffer_kb`. The registry tracks per-pid last-delivered offset so `read_output(since_offset?)` can default to "everything new since last read". `Workspace` owns the registry and the dtor terminates every still-running child before the workspace closes. | `ProcessRegistry`, `BackgroundProcess` |
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
| `src/agent/agent_mode.h` | S4.D types. `AgentMode` enum (`chat` / `plan` / `execute`) with string round-trip; `Plan` and `PlanStep` structs (`PlanStep::Status` = pending/in_progress/done/failed); `plan_to_json` helper. Header-only -- the actual mode + plan state lives on `AgentCore`. | `AgentMode`, `Plan`, `PlanStep` |
| `src/index/embedder.h/cpp` | llama.cpp session wrapper. Loads a GGUF model, tokenises via the embedded vocab, runs inference, returns an L2-normalised embedding vector. Dimension is read from the GGUF (`llama_model_n_embd`) and exposed via `dimensions()`; n_ctx defaults to 1024 (clamped to `n_ctx_train`). Workspace owns it as a `shared_ptr` so a process-wide `Workspace::set_embedder_provider` hook (used by the test suite) can hand the same instance to many Workspaces — single-Workspace production is unaffected. | `Embedder` |
| `src/index/reranker.h/cpp` | llama.cpp wrapper for cross-encoder GGUFs (e.g. `bge-reranker-v2-m3`) with `LLAMA_POOLING_TYPE_RANK`. `score(query, passage)` returns a single relevance logit; `score_batch()` is a convenience over many passages. Used by `search_semantic` / `search_hybrid` when `WorkspaceConfig::reranker_enabled` is true. | `Reranker` |
| `src/index/embedding_worker.h/cpp` | Background thread: consumes chunk queue, calls Embedder, writes embeddings to vectors_db (its own connection — no lock contention with the indexer writing to main_db). | `EmbeddingWorker` |
| `src/extractors/text_extractor.h` | Base interface for file format extractors (no .cpp — pure abstract + structs). | `ITextExtractor`, `ExtractionResult`, `ExtractedHeading` |
| `src/extractors/extractor_registry.h/cpp` | Maps file extension → `ITextExtractor`. Owned by `Workspace`. | `ExtractorRegistry` |
| `src/extractors/markdown_extractor.h/cpp` | Extracts text + `#` headings from `.md` files. | `MarkdownExtractor` |
| `src/extractors/html_extractor.h/cpp` | Extracts text + `<h1>`–`<h6>` headings from `.html` files. Strips `<script>`/`<style>`, decodes entities. | `HtmlExtractor` |
| `src/extractors/pdf_extractor.h/cpp` | Extracts text from `.pdf` files via PDFium. One pseudo-heading per page. Flags encrypted PDFs. | `PdfiumExtractor` |
| `src/extractors/docx_extractor.h/cpp` | Extracts text + `HeadingN` headings from `.docx` (unzip + parse `word/document.xml`). | `DocxExtractor` |
| `src/extractors/xlsx_extractor.h/cpp` | Extracts text from `.xlsx` sheets (resolves shared strings, emits one heading per sheet). | `XlsxExtractor` |
| `src/extractors/zip_reader.h/cpp` | miniz helper: read a single named entry from a .zip (shared by docx + xlsx). | `read_zip_entry()` |
| `src/core/frontend.h` | Frontend/core interfaces (no .cpp — pure abstract + enums). | `IFrontend`, `ILocusCore`, `ToolDecision`, `CompactionStrategy` |
| `src/core/frontend_registry.h` | Thread-safe frontend registry with exception-isolated fan-out. Header-only. | `FrontendRegistry` |
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
| `src/agent/file_change_tracker.h/cpp` | Thread-safe between-turn file-change tracker (S4.T). Holds `path -> mtime` snapshot + per-turn "agent touched" suppression set. `snapshot()` rebases against `IndexQuery::list_directory`; `diff_since_snapshot()` returns added/modified/deleted paths; `mark_agent_touched()` is called by `ToolDispatcher` for every `edit_file` / `write_file` / `delete_file` so the agent's own writes don't echo back as "external" next turn. `AgentCore` snapshots at construction + end-of-turn and diffs at top-of-turn, prepending `[Files changed since last turn: ...]` to the user message when non-empty. | `FileChangeTracker` |
| `src/agent/auto_commit.h/cpp` | S4.L per-turn auto-commit. Free function `auto_commit_after_turn(workspace_root, prefix, branch, message)` runs a small Win32 sync subprocess pipeline (separate from S4.I `RunCommandTool` -- short-lived `git ...` calls don't need Job Object / timeout overhead): detect `.git/` -> optional branch switch (creating + warning if missing) -> `git add -A` -> `git commit -F <tempfile>` (avoids cmd.exe quoting) -> `git rev-parse --short HEAD`. "Nothing to commit" stderr is treated as `skipped=true`, not failure. Called from `AgentCore::process_message` after the round loop when `change_tracker_->agent_touched_size() > 0` and `WorkspaceConfig::git_auto_commit` is true. | `AutoCommitResult`, `auto_commit_after_turn`, `make_commit_subject` |
| `src/agent/agent_core.h/cpp` | Orchestrator: agent thread + message queue + lifecycle, composing `AgentLoop` / `ToolDispatcher` / `ActivityLog` / `ContextBudget` / `SlashCommandDispatcher` / `FileChangeTracker`. Owns `ConversationHistory` (only writer) and the attached-context state. | `AgentCore` |
| `src/frontends/cli/cli_frontend.h/cpp` | Terminal frontend: token streaming, y/n/e tool approval, context meter, compaction prompts. | `CliFrontend` |
| `src/main.cpp` | CLI entry point. Arg parsing, logging init, REPL loop, Ctrl+C handler. | `CliArgs` |
| `src/frontends/gui/locus_app.h/cpp` | wxApp entry point. Owns Workspace, LLM, Agent lifetime. Single-instance check. | `LocusApp` |
| `src/frontends/gui/locus_frame.h/cpp` | Main window. wxAuiManager 3-pane layout, status bar, agent-event routing. Delegates menu bar to `MenuController`, status-pane composition to `OpsStatusView`. | `LocusFrame` |
| `src/frontends/gui/menu_controller.h/cpp` | Owns the wxMenuBar and its dynamic submenus (Recent Workspaces, Saved Sessions). All actions run through `Hooks` callbacks supplied by `LocusFrame`. | `MenuController` |
| `src/frontends/gui/ops_status_view.h/cpp` | Composes the right-pane status text from indexing/embedding progress counters. No wx dependencies beyond wxString. | `OpsStatusView` |
| `src/frontends/gui/chat_panel.h/cpp` | Chat UI: wxWebView (HTML/CSS), wxTextCtrl input, context meter footer. Streaming via wxTimer + md4c. S4.D adds a three-state `Chat / Plan / Execute` mode switcher above the input, an in-chat plan bubble (`.msg-plan` with status glyphs + Approve/Reject links via the `locus://` scheme intercepted in `on_webview_navigating`), and a `plan_chip_` next to the context meter. | `ChatPanel` |
| `src/frontends/gui/markdown.h/cpp` | md4c wrapper: markdown → HTML (GitHub-flavored dialect). | `markdown_to_html()` |
| `src/frontends/gui/locus_tray.h/cpp` | System tray icon. State display (idle/active/error), right-click menu, minimize-to-tray. | `LocusTray` |
| `src/frontends/gui/tool_approval_panel.h/cpp` | Tool approval: AUI bottom pane with JSON args (Scintilla), approve/modify/reject buttons, ask_user mode. | `ToolApprovalPanel` |
| `src/frontends/gui/wx_frontend.h/cpp` | IFrontend thread bridge: agent thread callbacks → wxThreadEvent → UI thread. | `WxFrontend` |
| `src/frontends/gui/metrics_view.h/cpp` | "Metrics" tab inside `ActivityPanel` (S4.S). Renders `MetricsAggregator::aggregates()` — text totals plus two pure-`wxDC` spark bars (turn duration, total tokens). Refreshes on a 1 s timer + on every appended activity event. "Export JSON…/CSV…" buttons pop a `wxFileDialog` so the user picks the destination. | `MetricsView` |
| `src/frontends/gui/compaction_dialog.h/cpp` | Context compaction modal: strategy B/C radio, N-turns slider, message preview, before/after token counts. | `CompactionDialog`, `CompactionChoice` |
| `src/frontends/gui/file_tree_panel.h/cpp` | File tree sidebar: wxTreeCtrl with lazy-loading from IndexQuery, index stats, re-index gauge. | `FileTreePanel` |
| `src/frontends/gui/settings_dialog.h/cpp` | Settings modal: LLM endpoint/model/temperature/context, index exclude patterns. | `SettingsDialog` |
| `src/frontends/gui/autostart.h/cpp` | Windows startup-on-login via HKCU Run key (opt-in). | `is_autostart_enabled()`, `set_autostart_enabled()` |

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
| [roadmap.md](roadmap.md) | Roadmap index — milestones M0–M6 with one-table summaries. Real detail lives in `roadmap/`. |
| [roadmap/M0.md](roadmap/M0.md), [M1.md](roadmap/M1.md), [M2.md](roadmap/M2.md) | Completed milestones — full task lists, one file per milestone |
| [roadmap/M3/](roadmap/M3/) | M3 Refactoring ✔ — one file per stage (S3.A–S3.L) |
| [roadmap/M4/](roadmap/M4/) | M4 Agent Quality — one file per stage (S4.A–S4.Y) |
| [roadmap/M5/](roadmap/M5/) | M5 Polish, UX & Performance — one file per stage (S5.A...) |
| [roadmap/M6/](roadmap/M6/) | M6 Connected — one file per stage (S6.1–S6.7) |
| [roadmap/backlog/README.md](roadmap/backlog/README.md) | Unscheduled tasks + parked drafts (e.g. S4.E LSP) — items recognised as worth doing but not yet promoted to a milestone |
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
| [architecture/mcp.md](architecture/mcp.md) | MCP integration: mcp.json schema, namespacing, approval/trust, lifecycle, result cap |
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
- **Update them whenever any user-observable agent behaviour changes** — that includes tools, extractors, the agent loop, the approval gate, the slash-command dispatcher (built-in slashes too: `/undo`, `/metrics`, `/export_metrics`, `/compact`, `/save`), the LLM stream decoder (`finish_reason` / refusal / mid-stream errors / tool-call validation), checkpoint and undo behaviour, telemetry surfaces (`MetricsAggregator`, `/metrics`, `/export_metrics`), and conversation/session state transitions. Schema changes, new tool, retired tool, changed arg shape, new extractor, new approval path, new slash command, new stream-decode event, new max_tokens / context-limit / timeout behaviour all need matching test coverage in the relevant `test_int_*.cpp`. Add a new `TEST_CASE` tagged `[integration][llm][<topic>]`; don't silently skip. If a feature can be exercised without an LLM round-trip (slash-only, deterministic), it still belongs in this suite tagged `[integration]` (drop `[llm]`) so the manual sweep runs it.
- **Do NOT run them by default.** They need a live LLM, take minutes, and are not part of the per-stage verification protocol above. Only run when explicitly asked ("run the integration tests", "run [search] integration").
- **When running on request, always pass `-console`.** The flag pops a dedicated console window and streams trace-level logs there live. The user wants to SEE the run (tool calls, LLM stream, SQL, FTS), not a silent 5-minute wait ending in a pass/fail summary. Omit `-console` only if the user explicitly asks for silent mode.
- **Consequence of `-console` when you launch via Bash/subprocess:** the exe redirects its stdout/stderr into the new window, so your captured output will look empty. The subprocess call still completes cleanly when tests finish (no pause). For pass/fail, rely on the exit code; for post-mortem detail point the user at `<workspace>/.locus/integration_test.log`.
- **Minimum verified model: Gemma 4 E4B @ 8k context.** Larger / more capable models are fine. Any model without tool-calling support will fail most cases.
- See [tests/integration/README.md](tests/integration/README.md) for the full run matrix, env vars, and harness design notes.

**Manual test plans (`tests/manual/`):**
- Whenever a stage ships a user-facing feature (GUI panel, new workflow, anything a non-developer can click through), write a QA-style manual test plan and drop it in `tests/manual/<feature-name>.md`. Filename is the feature as users see it (`mcp.md`, `plan-mode.md`, `checkpoints.md`), not the stage code (`S4.G`).
- Audience is QA already familiar with the project. Don't restate well-known project-level facts — how to launch Locus, what a workspace is, that only one Locus instance can attach to a workspace at a time, the standard `LM Studio at 127.0.0.1:1234 with a tool-calling model` setup. Skip Prerequisites entirely if there's nothing test-specific to call out.
- No build commands, no `src/` paths, no code references. Steps are GUI clicks or text-file edits. Each step has an Expected outcome; the doc includes Setup + Cleanup sections so a tester can run it in isolation.
- Add a row to [tests/manual/README.md](tests/manual/README.md)'s index table the same commit so the new plan is discoverable.
- These plans complement scripted suites — they catch what unit + integration tests deliberately don't (third-party-software workflows, drag-and-drop, visual layout regressions, error popups, UI state persistence across restarts).

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
