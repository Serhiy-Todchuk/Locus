# Locus Integration Tests

End-to-end tests that drive `AgentCore` against a **real local LLM** running in
LM Studio, exercising the full stack: workspace indexing, embeddings, the tool
catalog, the approval gate, file watchers, and the slash-command dispatcher.

Unlike `locus_tests` (fast, hermetic, Catch2-unit), this suite:

- **Needs a live LLM** — fails fast if LM Studio is not reachable.
- **Runs manually only** — not registered with `ctest` / `catch_discover_tests`,
  won't run on CI by default, won't run on `cmake --build` → test sweeps.
- **Uses the repo itself as its workspace** — opens `d:\Projects\AICodeAss\`
  as a live Workspace, so files, indexing, and semantic search are exercised
  against real content (not synthetic fixtures).

## What it covers

Tag filters match [Catch2 test tags](https://github.com/catchorg/Catch2/blob/devel/docs/command-line.md#specifying-which-tests-to-run).

| Tag | Source | What it exercises |
|---|---|---|
| `[smoke]`    | [test_int_smoke.cpp](test_int_smoke.cpp)        | LLM reachability gate, workspace opens + indexes, `read_file` via LLM. |
| `[search]`   | [test_int_search.cpp](test_int_search.cpp)      | Unified `search` tool — text (FTS5), regex (raw-content `std::regex`), symbols (tree-sitter), semantic (vectors), hybrid (RRF merge). |
| `[outline]`  | [test_int_outline.cpp](test_int_outline.cpp)    | `get_file_outline` against every extractor — `.md`, `.pdf` (PDFium), `.docx` (miniz+pugixml), `.xlsx`. |
| `[fs]`       | [test_int_fs_lifecycle.cpp](test_int_fs_lifecycle.cpp) | Full lifecycle in a scratch dir: `write_file` → `read_file` + `edit_file` (with `replace_all`) → indexer picks up change → `delete_file`. Also covers `list_directory`. |
| `[shell]`    | [test_int_shell.cpp](test_int_shell.cpp)        | `run_command` executes and captures stdout. |
| `[ask_user]` | [test_int_interactive.cpp](test_int_interactive.cpp) | `ask_user` routes through the approval gate, harness supplies a scripted response via `ToolDecision::modify`. |
| `[slash]`    | [test_int_slash.cpp](test_int_slash.cpp)        | `/help` and `/read_file` routed through `SlashCommandDispatcher`. |

Every test is also tagged `[integration][llm]` for bulk filtering.

## Prerequisites

### LLM

[LM Studio](https://lmstudio.ai/) (or any OpenAI-compatible server) running at
`http://127.0.0.1:1234` with a **tool-calling-capable** model loaded.

**Minimum verified model: Gemma 4 E4B @ 8k context.** That's the lower bar —
any larger / more capable model (Gemma 4 26B A4B, Qwen 3, etc.) will pass the
same suite and generally pass faster. 8k context is tight; some prompts
deliberately stay short so the system prompt + tool manifest + turn history
fit under the limit.

If you swap to a model without tool-calling support, most tests fail with
no `tool_called(...)` match — the LLM answers in plain text instead of
invoking the tool.

### Other

- No other `locus.exe` or `locus_gui.exe` process may be attached to the repo
  workspace while the tests run — the [WorkspaceLock](../../src/core/workspace_lock.h)
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
# All scenarios
build\release\tests\integration\Release\locus_integration_tests.exe

# One tag area at a time (recommended — fast feedback) with live log stream
build\release\tests\integration\Release\locus_integration_tests.exe "[smoke]" -console
build\release\tests\integration\Release\locus_integration_tests.exe "[search]"
build\release\tests\integration\Release\locus_integration_tests.exe "[outline]"
build\release\tests\integration\Release\locus_integration_tests.exe "[fs]"
build\release\tests\integration\Release\locus_integration_tests.exe "[shell]"
build\release\tests\integration\Release\locus_integration_tests.exe "[ask_user]"
build\release\tests\integration\Release\locus_integration_tests.exe "[slash]"

# List registered tests
build\release\tests\integration\Release\locus_integration_tests.exe --list-tests
```

### Configuration

Environment variables — they won't clash with Catch2's argument parser:

| Env var | Default | Purpose |
|---|---|---|
| `LOCUS_INT_TEST_ENDPOINT`  | `http://127.0.0.1:1234` | LM Studio URL. |
| `LOCUS_INT_TEST_MODEL`     | empty (server default)  | Model id (e.g. `google/gemma-4-e4b-instruct`). |
| `LOCUS_INT_TEST_WORKSPACE` | compile-time repo root  | Override to point at a different workspace folder. |

CLI flags (parsed by our custom `main` before Catch2 sees the rest of argv):

| Flag | Effect |
|---|---|
| `-console` | **Always pops a dedicated console window** (titled *"Locus Integration Tests"*) and streams every trace/info log line there in real time — tool calls, LLM stream, FTS queries, SQL, embedding progress, the lot. If the exe was launched from a real console (cmd.exe, Windows Terminal), that console is used and stays visible afterwards. If launched from anything else (IDE Run, double-click, agent subprocess, a redirected pipe), the exe self-relaunches via `CreateProcessW(CREATE_NEW_CONSOLE)` so a fresh window pops regardless — the parent process just waits on the child and propagates its exit code. The window closes as soon as tests finish — for a post-mortem read `.locus/integration_test.log`, which captures the full trace unconditionally. |

Without `-console`, stderr stays at warning-level only and the Catch2 output
is summary-only. Logs always land in `<workspace>/.locus/integration_test.log`
(rotating, full trace level) regardless of `-console`.

### Guidance for Claude (AI assistant)

When the user asks you to run the integration tests, **always pass `-console`**
unless they explicitly say otherwise. The user wants to see what's happening
in real time; the silent default is for headless / CI-style runs.

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
  case — otherwise a 20-case run would re-index the repo 20×.
- **All tools forced to `ask` policy.** The fixture overrides
  `Workspace::config().tool_approval_policies` to route every tool call
  through `on_tool_call_pending`, regardless of its default policy. The
  harness then auto-approves synchronously (no real round-trip latency).
  Reason: auto-approve tools (`read_file`, `search`, `list_directory`,
  `get_file_outline`) skip `on_tool_call_pending` in normal operation — so
  tests couldn't observe them. Forcing `ask` gives uniform observability.
- **Deterministic vs LLM-driven assertions.** When verifying system-level
  behavior (indexer picked up a new file, DB contains a symbol), the tests
  call `IndexQuery` directly rather than prompting the LLM to do a search.
  LLM-driven search coverage lives in `[search]` against stable repo content
  where prompts are reproducible.
- **Scratch directory.** Tests that create/modify files use
  `tests/integration_tmp/` (gitignored). Wiped at harness startup and shutdown
  — aborted runs never leak state.

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
       PromptResult r = h.prompt("ask the LLM to do the thing");
       REQUIRE_FALSE(r.timed_out);
       REQUIRE(r.tool_called("expected_tool"));
   }
   ```
4. Assert on **observable behavior** (tool invoked, file on disk, index
   updated) — not on exact LLM prose. Local models are non-deterministic.
5. Where prompt wording matters for the LLM to choose the right tool, spell
   it out: "Use the search tool in text mode…" beats "search for…". Small
   models especially respond to explicit tool naming.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Fixture throws *"LM Studio not reachable at …"* | Server not running, wrong port, or `/v1/models` returns empty. |
| Fixture throws *"workspace already held by another process"* | Close `locus.exe` / `locus_gui.exe` for this repo first (WorkspaceLock). |
| Most assertions `tool_called(...)` fail | Loaded model doesn't support tool/function calling. Load one that does. |
| `[fs]` edit section fails with *"'old_string' not found"* | LLM capitalised the word differently than the prompt prescribed. The prompt spells the exact casing — extend it if your model drifts. |
| `[outline]` DOCX shows 0 entries | Expected — the Apache POI SampleDoc.docx has no explicit `HeadingN` styles. The test asserts the tool ran, not a specific heading. |

Full traces (tool args, LLM stream, FTS queries, embedding progress) are in
`<workspace>/.locus/integration_test.log`.
