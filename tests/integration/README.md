# Locus Integration Tests

End-to-end tests that drive `AgentCore` against a **real local LLM** running in
LM Studio, exercising the full stack: workspace indexing, embeddings, the tool
catalog, the approval gate, file watchers, and the slash-command dispatcher.

Unlike `locus_tests` (fast, hermetic, Catch2-unit), this suite:

- **Needs a live LLM** -- fails fast if LM Studio is not reachable.
- **Runs manually only** -- not registered with `ctest` / `catch_discover_tests`,
  won't run on CI by default, won't run on `cmake --build` -> test sweeps.
- **Uses the repo itself as its workspace** -- opens `d:\Projects\AICodeAss\`
  as a live Workspace, so files, indexing, and semantic search are exercised
  against real content (not synthetic fixtures).

## What it covers

Tag filters match [Catch2 test tags](https://github.com/catchorg/Catch2/blob/devel/docs/command-line.md#specifying-which-tests-to-run).

| Tag | Source | What it exercises |
|---|---|---|
| `[smoke]`    | [test_int_smoke.cpp](test_int_smoke.cpp)        | LLM reachability gate, workspace opens + indexes, `read_file` via LLM. |
| `[search]`   | [test_int_search.cpp](test_int_search.cpp)      | Five per-mode search tools (S6.17 Task G / ADR-0008; ADR-0009 retired `search_hybrid`) -- `search_text` (FTS5), `search_regex` (raw-content `std::regex`), `search_symbols` (tree-sitter), `search_ast` (Tree-sitter S-expression queries), `search_semantic` (vectors). |
| `[outline]`  | [test_int_outline.cpp](test_int_outline.cpp)    | `get_file_outline` against every extractor -- `.md`, `.pdf` (PDFium), `.docx` (miniz+pugixml), `.xlsx`. |
| `[fs]`       | [test_int_fs_lifecycle.cpp](test_int_fs_lifecycle.cpp) | Full lifecycle in a scratch dir: `write_file` -> `read_file` + `edit_file` (with `replace_all`) -> indexer picks up change -> `delete_file`. Also covers `list_directory`. |
| `[shell]`    | [test_int_shell.cpp](test_int_shell.cpp)        | `run_command` executes and captures stdout. |
| `[bg]`       | [test_int_bg.cpp](test_int_bg.cpp)              | S4.I background-process tools -- `run_command_bg` + `read_process_output` + `list_processes` + `stop_process` chained in single turns. |
| `[ask_user]` | [test_int_interactive.cpp](test_int_interactive.cpp) | `ask_user` routes through the approval gate, harness supplies a scripted response via `ToolDecision::modify`. |
| `[slash]`    | [test_int_slash.cpp](test_int_slash.cpp)        | `/help` and `/read_file` routed through `SlashCommandDispatcher`. |
| `[s4.x]`     | [test_int_prompt_templates.cpp](test_int_prompt_templates.cpp) | Prompt templates -- `/reload` picks up a fresh `.md`, `/<name> <arg>` expands and lands as a user message that the live LLM responds to; unknown slash surfaces an error without an LLM round. |
| `[file_change_awareness]` | [test_int_file_change_awareness.cpp](test_int_file_change_awareness.cpp) | S4.T -- external user edits between turns surface as `[Files changed since last turn: ...]` prefix on the next user message; agent's own writes don't echo back. |
| `[undo]`     | [test_int_undo.cpp](test_int_undo.cpp)          | S4.B -- `/undo` slash restores files after `write_file` / `edit_file`; reports a no-op when no checkpointed turns remain. |
| `[metrics]`  | [test_int_metrics.cpp](test_int_metrics.cpp)    | S4.S -- `/metrics` summarises tokens + per-tool counts after a real turn; `/export_metrics json|csv` writes a file under `.locus/metrics/`; bogus format reports an error. |
| `[max_tokens]` | [test_int_max_tokens.cpp](test_int_max_tokens.cpp) | S4.U -- forces `finish_reason="length"` with `max_tokens=200`; asserts a clear `on_error` is fired. Multi-tool truncation case asserts no malformed tool call (empty / unparseable arguments) escapes the LLMClient gate into history. |
| `[plan]`     | [test_int_plan.cpp](test_int_plan.cpp)          | S4.D plan mode -- `set_mode(plan)` filters the API tool catalog to `propose_plan` only; full `propose -> approve -> execute` flow with `on_plan_proposed` / `on_mode_changed` / `on_plan_step_advanced` event capture; reject path keeps mode=plan and clears the plan. Soft-skips assertions that depend on small-model cooperation (e.g. consistent `mark_step_done` calls). |
| `[mcp]`      | [test_int_mcp.cpp](test_int_mcp.cpp)            | S4.G MCP client -- spins up a self-contained `LocusSession` on a temp workspace whose `mcp.json` points at the bundled `locus_mock_mcp_server`; asks the live LLM to invoke `mcp:mock:echo` with a specific text argument; verifies the call routes through `ToolDispatcher -> McpTool -> McpClient -> stdio -> mock -> response` and the echoed text reaches the tool result. Soft-skips the round-trip assertions if the model doesn't pick the namespaced MCP tool (protocol mechanics are covered by `tests/test_mcp.cpp` at unit level). |
| `[history_delete]` | [test_int_history_delete.cpp](test_int_history_delete.cpp) | S5.G -- `on_history_message_added` fires with the right role + deletable flag after a real user/assistant turn; `AgentCore::delete_message` queues onto the agent thread, drops the chosen message, and broadcasts `on_history_message_deleted`; system message + unknown ids are refused without side effects. |
| `[session-restore]` | [test_int_session_restore.cpp](test_int_session_restore.cpp) | S6.15 -- one live tool-using turn (read_file + final answer), then reopen the session in a fresh `LocusSession` via `add_tab_for_session`; every non-system `ChatMessage` field (`content`, `reasoning_content`, `tool_calls[].id/name/arguments`, `tool_call_id`) must match the in-memory baseline byte-for-byte. Asserts at least one assistant tool-call turn and one tool-result row actually fired so the test exercises the round-trip rather than passing trivially. |
| `[s6.10]`    | [test_int_tool_calls.cpp](test_int_tool_calls.cpp), [test_int_small_model_robustness.cpp](test_int_small_model_robustness.cpp) | S6.10 small-model robustness pass. `[json_repair]` exercises Task A's repair pre-pass on three malformed inputs (trailing comma + unquoted keys, surrounding prose + missing closer, `arguments` wrapper). `[grammar]` exercises Task D's BestEffort retry intercept with a separate LMStudioClient. `[truncation]` exercises Task G -- WriteFileTool + EditFileTool refuse bodies containing elision markers, the dispatcher emits `truncation_blocked`. `[quality]` exercises Task B -- `QualityMonitor::evaluate` returns std::nullopt on a healthy turn. `[presets][matcher]` exercises Task F -- `find_preset_for_model` resolves the harness's currently-loaded model. `[examples]` asserts Task E examples land in the full-manifest schema. `[thinking_strip]` exercises Task C -- assistant `reasoning_content` is exposed after a live turn. |
| `[s6.17][compaction-replay]` | [test_int_compaction_replay.cpp](test_int_compaction_replay.cpp) | S6.17 Task B Pass-5-replay regression. Loads the four archived Pass-5 histories at `tests/ui_automation/output/agentic_TestLocalVibe2/history.before-compact-*.json` (the exact pre-compaction states from the 2026-05-25 baseline that produced `reached=no` three times in a row) and runs `CompactionPipeline::run` with `aggressiveness=balanced` + the harness LLM for the summary layer. Asserts each archive's compaction reaches the original target (9831 tokens) -- the regression guard for the F17 finding that motivated the whole stage. |
| `[s6.17][edit_file_shorthand]` | [test_int_edit_file_shorthand.cpp](test_int_edit_file_shorthand.cpp) | S6.17 Task E -- the `edit_file` single-edit shorthand (top-level `old_string`+`new_string` synonym for `edits=[{...}]`) round-trips end-to-end against a live LLM. Read-then-edit is split into separate `h.prompt(...)` calls so small-model variance doesn't conflate the two `tool_called` assertions. |
| `[s6.17][read_file_alias]` | [test_int_read_file_alias_rejection.cpp](test_int_read_file_alias_rejection.cpp) | S6.17 Task D -- `read_file` rejects the legacy `lines` alias and the rejection message names the canonical `offset`+`length` pair so the small-model retry lands the right shape. |
| `[s6.11][lazy_manifest]` | [test_int_lazy_manifest.cpp](test_int_lazy_manifest.cpp) | S6.11 -- `describe_tool` returns a valid OpenAI-format schema entry for a known tool when `lazy_tool_manifest=true`, and emits a closest-match suggestion for an unknown name. Plus mid-session toggle of `lazy_tool_manifest` round-trips a basic turn (no stuck history, no stream wedge). |
| `[s6.17][prompt_cost]` | [test_int_lazy_manifest.cpp](test_int_lazy_manifest.cpp) | S6.17 Task H -- the `prompt_cost` preset ("balanced") resolves to the right `lazy_tool_manifest` + `system_prompt_profile` pair on load and shrinks the per-turn API tools-array token count vs the verbose preset. |
| `[s6.13][reasoning_watchdog]` | [test_int_reasoning_watchdog.cpp](test_int_reasoning_watchdog.cpp) | S6.13 -- auto-nudge cancels the in-flight LLM stream and injects the "Stop reasoning, commit now" steering message when `reasoning_max_chars` / `reasoning_max_seconds` trips; 2-nudge cap aborts the turn with the documented "Agent appears stuck" message. Companion case asserts the watchdog stays disabled by default and a long answer completes normally. |
| `[tabs]` | [test_int_tabs_and_sessions.cpp](test_int_tabs_and_sessions.cpp) | S5.I multi-tab + session lifecycle -- per-tab agent isolation (tab B doesn't see tab A's history), lazy session-file creation (empty tab leaves no JSON), auto-save after first user message with derived title, close_tab persists non-empty / discards empty, delete_tab removes JSON + archive subdirectory, legacy session JSON loads with derived title. Sub-tags `[tabs][close]`, `[tabs][delete]`, `[tabs][persistence]`, `[tabs][legacy]` for selective runs. |

Every test is also tagged `[integration][llm]` for bulk filtering.

## Prerequisites

### LLM

[LM Studio](https://lmstudio.ai/) (or any OpenAI-compatible server) running at
`http://127.0.0.1:1234` with a **tool-calling-capable** model loaded.

**Minimum verified model: Gemma 4 E4B @ 8k context.** That's the lower bar --
any larger / more capable model (Gemma 4 26B A4B, Qwen 3, etc.) will pass the
same suite and generally pass faster. 8k context is tight; some prompts
deliberately stay short so the system prompt + tool manifest + turn history
fit under the limit.

If LM Studio has several models loaded, the harness picks whichever one the
server returns first from `/v1/models`. Pin a specific id via
`LOCUS_INT_TEST_MODEL=qwen/qwen3.6-27b` (or similar) when you want determinism
across runs.

**Capability gate.** Against LM Studio, the harness probes `/api/v0/models/<id>`
at startup and refuses to construct if the loaded model doesn't advertise the
`tool_use` capability -- the whole suite is tool-driven and would otherwise
fail deep in the agent loop with a confusing HTTP 400. Non-LM-Studio backends
(Ollama, llama-server) don't implement the v0 endpoint; the harness treats
that as "unknown" and proceeds without enforcement, so the manual rule still
applies: load a tool-calling-capable model or expect failures.

### Other

- No other `locus.exe` or `locus_gui.exe` process may be attached to the repo
  workspace while the tests run -- the [WorkspaceLock](../../src/core/workspace_lock.h)
  refuses a second attach.

## Build

Part of the normal CMake configure. The target is **off the default build**:
pass it explicitly.

```
cmake --build build/release --config Release --target locus_integration_tests
```

Output: `build/release/tests/integration/Release/locus_integration_tests.exe`
(with `pdfium.dll` copied next to it).

## Run

```
# All scenarios (inline output)
build\release\tests\integration\Release\locus_integration_tests.exe

# One tag area at a time (recommended -- fast feedback)
build\release\tests\integration\Release\locus_integration_tests.exe "[smoke]"
build\release\tests\integration\Release\locus_integration_tests.exe "[search]"
build\release\tests\integration\Release\locus_integration_tests.exe "[outline]"
build\release\tests\integration\Release\locus_integration_tests.exe "[fs]"
build\release\tests\integration\Release\locus_integration_tests.exe "[shell]"
build\release\tests\integration\Release\locus_integration_tests.exe "[bg]"
build\release\tests\integration\Release\locus_integration_tests.exe "[ask_user]"
build\release\tests\integration\Release\locus_integration_tests.exe "[slash]"
build\release\tests\integration\Release\locus_integration_tests.exe "[undo]"
build\release\tests\integration\Release\locus_integration_tests.exe "[metrics]"
build\release\tests\integration\Release\locus_integration_tests.exe "[max_tokens]"

# Pop a foreground console window when a human wants to watch the trace
# stream live (see -console below).
build\release\tests\integration\Release\locus_integration_tests.exe "[search]" -console

# List registered tests
build\release\tests\integration\Release\locus_integration_tests.exe --list-tests
```

### Configuration

Environment variables -- they won't clash with Catch2's argument parser:

| Env var | Default | Purpose |
|---|---|---|
| `LOCUS_INT_TEST_ENDPOINT`  | `http://127.0.0.1:1234` | LM Studio URL. |
| `LOCUS_INT_TEST_MODEL`     | empty (server default)  | Model id (e.g. `google/gemma-4-e4b-instruct`). |
| `LOCUS_INT_TEST_WORKSPACE` | compile-time repo root  | Override to point at a different workspace folder. |

CLI flags (parsed by our custom `main` before Catch2 sees the rest of argv):

| Flag | Effect |
|---|---|
| `-console` | **Pops a dedicated console window** (titled *"Locus Integration Tests"*) and streams every trace/info log line there in real time -- tool calls, LLM stream, FTS queries, SQL, embedding progress, the lot. If the exe was launched from a real console (cmd.exe, Windows Terminal), that console is used and stays visible afterwards. If launched from anything else (IDE Run, double-click, agent subprocess), the exe self-relaunches via `CreateProcessW(CREATE_NEW_CONSOLE)` so a fresh window pops regardless -- the parent process just waits on the child and propagates its exit code. Caveat: stdout/stderr go to the new console, so a subprocess capture comes back empty -- use this only when a human is watching. |

Without `-console` (the default), Catch2 + warning-level log lines stream
inline to stdout/stderr -- ideal when the runner needs to read failures
back (CI, scripts, an AI assistant pumping the suite). Full trace lines
always land in `<workspace>/.locus/integration_test.log` regardless.

### Guidance for Claude (AI assistant)

Run **inline (no `-console`)** by default so failures stream back through the
subprocess pipe and can be root-caused in the same turn. Filter llama.cpp
boot noise with `| grep -vE "^llama_|^print_info|^load|^ggml_|^create_tensor|^repack|^graph_reserve|^set_abort"`
when only Catch2 / harness lines matter. Reach for `-console` only when the
user explicitly asks for the foreground pop-out so they can watch it live.

## Architecture

Three layers, all in this directory:

```
harness_frontend.{h,cpp}     # IFrontend impl: auto-approves, records events,
                             #   feeds scripted ask_user answers, signals
                             #   turn completion via condvar
harness_fixture.{h,cpp}      # Singleton that owns Workspace + LLMClient +
                             #   AgentCore lifetime for the whole session
session_listener.cpp         # Catch2 EventListener that tears the harness
                             #   down at session end
test_int_*.cpp               # Per-scenario TEST_CASEs
```

Design notes:

- **Shared harness, one process, one agent thread.** Catch2 TEST_CASEs share
  a lazy-initialized `IntegrationHarness` singleton. The repo is indexed once
  (~1500 files, ~1700 embeddings on first run) and then reused across every
  case -- otherwise a 20-case run would re-index the repo 20x.
- **Conversation history persists across cases in the same file.** The
  Catch2 `IntegrationSessionListener` calls `h.agent().reset_conversation()`
  only when the source FILE changes, so sibling cases in the same
  `test_int_*.cpp` see each other's tool_calls. **This bites tests that
  assert on `r.tool_called(X)` for an identical call sibling cases also
  make** -- `QualityMonitor::evaluate` (S6.10 Task B) walks backwards
  through history, finds the previous matching call, and injects a
  `repeated_tool_call` nudge instead of dispatching. Net effect:
  `r.tool_called(X)` returns false, the assertion fails on whichever random
  order Catch2 picks. **Fix**: call `h.agent().reset_conversation()` at the
  top of each case that needs a clean history -- see
  `test_int_metrics.cpp` / `test_int_edit_file_shorthand.cpp` /
  `test_int_read_file_alias_rejection.cpp` for the pattern. Or, when the
  test inherently exercises multiple tool calls in sequence and small-model
  variance makes one-prompt-multiple-calls flaky, split into separate
  `h.prompt(...)` calls (see edit_file_shorthand's read-then-edit split).
- **All tools forced to `ask` policy.** The fixture overrides
  `Workspace::config().tool_approval_policies` to route every tool call
  through `on_tool_call_pending`, regardless of its default policy. The
  harness then auto-approves synchronously (no real round-trip latency).
  Reason: auto-approve tools (`read_file`, `search_text` / `search_regex` /
  `search_symbols` / `search_semantic` / `search_ast`, `list_directory`,
  `get_file_outline`) skip `on_tool_call_pending` in normal operation --
  so tests couldn't observe them. Forcing `ask` gives uniform observability.
- **Deterministic vs LLM-driven assertions.** When verifying system-level
  behavior (indexer picked up a new file, DB contains a symbol), the tests
  call `IndexQuery` directly rather than prompting the LLM to do a search.
  LLM-driven search coverage lives in `[search]` against stable repo content
  where prompts are reproducible.
- **Scratch directory.** Tests that create/modify files use
  `tests/integration_tmp/` (gitignored). Wiped at harness startup and shutdown
  -- aborted runs never leak state.
- **Gitignore respect overridden in the harness.** The repo's `.gitignore`
  lists `tests/integration_tmp/`, which would otherwise hide every scratch
  file from the indexer (S4.L respects the workspace's gitignore by default).
  The harness flips `workspace_->config().respect_gitignore = false` +
  `indexer().reload_gitignore()` immediately after Workspace construction,
  so any file written under the scratch dir during a test gets indexed and
  surfaces through `IndexQuery` / `FileChangeTracker`. Process-local only --
  the on-disk `.locus/config.json` is never rewritten by the harness.

## Writing a new scenario

1. Pick an existing `test_int_*.cpp` by topic, or add a new file and list it
   in [CMakeLists.txt](CMakeLists.txt).
2. Tag with `[integration][llm]` **plus** a topic tag.
3. Grab the fixture:
   ```cpp
   #include "harness_fixture.h"
   using namespace locus::integration;

   TEST_CASE("my scenario", "[integration][llm][my_topic]") {
       auto& h = harness();
       h.agent().reset_conversation();  // see "Conversation history
                                         // persists across cases" above --
                                         // skip only when the case is
                                         // genuinely first-in-file or
                                         // doesn't assert on tool_called.
       PromptResult r = h.prompt("ask the LLM to do the thing");
       REQUIRE_FALSE(r.timed_out);
       REQUIRE(r.tool_called("expected_tool"));
   }
   ```
4. Assert on **observable behavior** (tool invoked, file on disk, index
   updated) -- not on exact LLM prose. Local models are non-deterministic.
5. Where prompt wording matters for the LLM to choose the right tool, spell
   it out: "Use the search_text tool..." beats "search for...". Small
   models especially respond to explicit tool naming. Avoid chaining two
   tool calls in one prompt against small models -- split into two
   `h.prompt(...)` calls (each round verifies one tool_called); see
   `test_int_edit_file_shorthand.cpp` for the pattern.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Fixture throws *"LM Studio not reachable at ..."* | Server not running, wrong port, or `/v1/models` returns empty. |
| Fixture throws *"workspace already held by another process"* | Close `locus.exe` / `locus_gui.exe` for this repo first (WorkspaceLock). |
| Most assertions `tool_called(...)` fail | Loaded model doesn't support tool/function calling. Load one that does. |
| `[fs]` edit section fails with *"'old_string' not found"* | LLM capitalised the word differently than the prompt prescribed. The prompt spells the exact casing -- extend it if your model drifts. |
| `[outline]` DOCX shows 0 entries | Expected -- the Apache POI SampleDoc.docx has no explicit `HeadingN` styles. The test asserts the tool ran, not a specific heading. |
| `[fs]` / `[file_change_awareness]` fail with no notes.md / s4t_external.txt in the index | The harness should be flipping `respect_gitignore=false` (see Design notes above). If you've tweaked harness ctor or moved tmp_dir somewhere else, verify the flip still runs before any test prompts. The repo's `.gitignore` excludes `tests/integration_tmp/`. |

Full traces (tool args, LLM stream, FTS queries, embedding progress) are in
`<workspace>/.locus/integration_test.log`.
