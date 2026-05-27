# Agentic Testing -- driving Locus interactively from another LLM

Agentic testing exposes the existing UIA framework as a long-lived TCP/JSON
service. Instead of canned `.json` scripts, a QA-LLM (typically a Claude
session in another terminal) sends one op at a time, observes the result,
and decides what to do next. The point: a real LLM can react to whatever
Locus actually does -- explore failure modes a deterministic script can't
anticipate, probe how small local models behave under realistic load, file
issues at a higher level than per-widget assertions.

This document is written for **future Claude sessions** -- a teammate who
walks in cold, reads this, and is expected to drive Locus through a real
end-to-end scenario without further hand-holding. Read it through once
before you start; you will not be re-reading parts of it mid-session.

---

## When to use this vs. the other test suites

| You want to ... | Use |
|---|---|
| Verify a specific widget exists / is clickable | scripted UIA (`scripts/*.json`) |
| Verify a specific agent flow round-trips through a real LLM | `tests/integration/` |
| Drive Locus through a realistic user scenario and report what broke | **this** -- agentic mode |
| Probe how a small local model handles a class of prompts | **this** |
| Test deterministic chat / settings / persistence | scripted UIA |

Agentic mode is the only one of the four where the test itself is
non-deterministic. That is the point.

---

## One-time setup

1. **Build the harness.** From `d:/Projects/AICodeAss/`:

    ```
    cmake --build build/release --config Release --target locus_ui_tests
    ```

   Output: `build/release/tests/ui_automation/Release/locus_ui_tests.exe`.
   The `locus_gui` target is built implicitly.

2. **LM Studio.** Make sure LM Studio (or another OpenAI-compatible local
   server) is reachable at the address Locus is configured to use --
   default is `http://127.0.0.1:1234`. Load the model you actually want
   to test (the whole point is testing how *this* model handles things).

3. **Desktop session.** UIA needs a real desktop. The GUI window will
   pop, take focus, and intercept keystrokes. Run on the machine the
   user left you on, not over a headless SSH session.

---

## Launching the agentic server

```
build\release\tests\ui_automation\Release\locus_ui_tests.exe ^
    --agentic ^
    --workspace tmp ^
    --port 7878
```

Arguments:

| Flag | Meaning | Default |
|---|---|---|
| `--agentic` | Required; switches from scripted to interactive mode. | -- |
| `--workspace <path|tmp>` | `tmp` creates a fresh dir under `%TEMP%/locus_ui_tests/agentic_<rand>/`. An absolute path opens an existing workspace as-is. | required |
| `--port N` | Localhost TCP port the server listens on. `0` picks an OS-assigned free port; read the chosen port from `agentic.port` in the output dir. | 7878 |
| `--allow-first-time-prompts` | Skip pre-seeding `.locus/config.json`. The capabilities first-open modal will fire on launch. | off |
| `--no-seed-config` | Skip writing the default `.locus/config.json` even for `tmp` workspaces. Use when you want to write a custom config first via `write_workspace_file`. | off (seed by default for tmp) |
| `--output-root <dir>` | Where per-session artifacts land. | bundled `output/` dir |
| `--gui <path>` | Override the location of `locus_gui.exe`. | baked into the binary |
| `--bind-host <ip>` | Interface to bind. Keep it on localhost. | `127.0.0.1` |

When the server is ready it writes one line to stdout:

```
agentic-server: listening on 127.0.0.1:7878 workspace=C:\...\agentic_xyz
```

and the matching port to `<output_root>/agentic_<wslabel>/agentic.port`.
The output dir also gets `agentic.log` -- a timestamped op-by-op record.

### What agentic mode turns on automatically

The `--agentic` flag isn't just a TCP server. When the harness spawns
`locus_gui.exe` via the `launch` op it also passes `--agentic-mute-noise`,
which:

- Demotes the `db` (SQL exec / prepare) and `fs` (file watcher, indexer,
  list_directory) loggers from trace to info, so `.locus/locus.log` stops
  drowning the LLM / tool / agent signal you actually want to read.
- Drops the spdlog flush threshold to info, so `read_locus_log` between
  rounds sees fresh content instead of buffered-for-warn output.
- The first log line is always `Noise loggers muted: db, fs (trace -> info); flush_on=info`
  so you can confirm the harness wired itself up.

Independently, every agentic launch benefits from the post-Tetris-findings
instrumentation that lives in the default logger:

- `LLM stream: first chunk after X ms` / `inter-chunk gap X ms` (warn at >60s)
  / end-of-stream summary (chunks, bytes, max gap) -- pinpoints "model slow"
  vs "model stalled" without needing -verbose.
- `LLM stream: chunk #N (+B B, gap G ms, total T B)` -- info-level heartbeat
  fires every 32 chunks OR every 10 s (whichever first) so a long reasoning
  round writes to the log even when the chat WebView shows nothing visible.
  Closes the TestLocalVibe finding where `wait_for_agent_idle` reported
  `status: "stuck"` during 25-minute reasoning rounds because the previous
  per-chunk trace was gated at trace level (and `--agentic-mute-noise` drops
  the spdlog flush threshold to info, so trace lines stayed buffered).
- `run_command[pid=N]: spawned` / `child exited` / `reader joined after Y ms`,
  plus a 30s heartbeat that fires only when the reader is still draining
  after the child exited (the signature of the inherited-pipe leak from
  findings 7+8).
- `AgentCore: round N start (turn T)` info line bookends every LLM round.
- `tool 'X' result: ... exec_ms=Y` for every tool dispatch.

### What the agentic seed config sets

The default `tmp` workspace (or any workspace where `--no-seed-config`
isn't passed) gets a `.locus/config.json` with these knobs pre-set:

- `tool_approvals` -- write/edit/delete/run_command/run_command_bg all `auto`,
  so the agent isn't blocked on a modal you can't click from the QA-LLM driver.
- `index.semantic_search.enabled = false` -- skip the embedder load cost,
  keeps tmp-workspace bring-up fast. Pair with `capabilities.semantic_search
  = false` (also seeded off) so the tool description matches.
- `capabilities.background_processes = true` -- enable the `run_command_bg`
  / `read_process_output` / `stop_process` / `list_processes` tools so
  Task 5-style "test stdin to a long-running process" scenarios work
  out of the box. Without this the agent silently falls back to the
  synchronous `run_command` with the 30 min default timeout.
- `capabilities.memory_bank = true` -- ships on by default; agentic tests
  for `/memory add` + `/memory search` round-trip exercise this path.
- `sessions.auto_cleanup_enabled = false`, `sessions.restore_last = false`
  -- each `launch` starts a fresh session; prior session bodies stay on
  disk for the harness to read via `dump_history` / `read_workspace_file`.

### Recovery knobs you may want to set in `.locus/config.json`

| Key | Default | What it does |
|---|---|---|
| `llm.timeout_ms` | 1800000 (30 min) | Stream-stall watchdog. The default was bumped from 600000; raise further for very slow local models. Also editable via Settings -> LLM -> Stream stall timeout. |
| `agent.tool_max_runtime_s` | 0 (off) | Defence-in-depth wall-clock guardrail on every `tool->execute` call. When >0, the dispatcher trips `cancel_flag_` after that many seconds and synthesizes a "tool timed out" result. Recommended starter value if you hit the `run_command` hang: 600. |
| `agent.dump_on_run_command_hang` | false | When the reader-heartbeat catches a stuck `run_command` after child exit, shell out to `procdump.exe -ma <self_pid> .locus/dumps/<...>.dmp` so the agent-thread stack survives a force-kill. Requires Sysinternals `procdump.exe` on PATH. |
| `agent.max_rounds_per_message` | 500 | Hard cap on tool-call rounds per user message. Was a hardcoded 20 until small-model build-fix loops kept hitting it (agentic Tetris run 2 stopped at round 20 mid-edit). Default is high enough that realistic "read 200 files / edit 50" workloads don't trip it; set to `0` to remove the cap entirely. The chat-footer "round N/M" chip surfaces the in-flight count, and the `on_error` message when the cap fires now names this knob. |
| `agent.run_command_truncate_lines` | 50 | Default head + tail line count `run_command` / `read_process_output` keep before sending output back to the LLM, when the call doesn't pass explicit `output_filter_lines`. The middle is replaced with `[... K lines elided ...]` and the full pre-filter body lands in `.locus/locus.log` at trace level. Set to `0` to disable smart-truncate (raw output flows back, capped only by the 1 MB hard backstop). Per-call `output_filter_mode` / `_pattern` / `_lines` / `_context` override the default for one call. |
| `agent.lazy_tool_manifest` **(S6.11)** | false | Collapses the per-turn tool catalog (both system-prompt `## Available Tools` and the API `tools: [...]` array) to one-line summaries; the model fetches the full schema via the new `describe_tool('<name>')` meta-tool. Saves ~2-3K tokens per turn on the default 12-tool roster. Recommended for any local model running under 32k context. Costs one extra round-trip the first time the model calls an unfamiliar tool in a session. |
| `agent.system_prompt_profile` **(S6.12)** | "full" | Which prose body the system prompt renders. `"full"` keeps today's verbose Rules / Editing / Shell / MSVC sections (~700-1000 t). `"compact"` collapses to load-bearing rules only, no examples (~300 t). `"minimal"` keeps only the invariants a fine-tuned agent must be told (~80 t, power-user opt-in). Workspace metadata / LOCUS.md / memory bank / tools section / format addendum are identical across profiles. Pairs with `lazy_tool_manifest` as a "Prompt Cost" tuning group; ADR-0007 has the matrix. |
| `agent.reasoning_max_seconds` **(S6.13)** | 0 (off) | Wall-clock seconds the LLM may stream in a single round before the reasoning watchdog trips. 0 = off. Companion to `reasoning_max_chars` (OR-semantics: whichever fires first wins). Recommended starting value for Qwen 3.x / DeepSeek-R1-distill: 120. |
| `agent.reasoning_max_chars` **(S6.13)** | 0 (off) | Combined reasoning + text channel chars per round before the watchdog trips. 0 = off. ~30000 is a reasonable starting value -- the TestLocalVibe Qwen 27B Task 2 burned 666 KB before the harness aborted, so 30K is a comfortable cap that catches genuine spirals well before the cliff. |
| `agent.reasoning_auto_nudge` **(S6.13)** | false | When true, the watchdog automatically cancels the in-flight LLM stream and injects a "Stop reasoning, commit to a tool call now" steering message instead of waiting for the user to click the chat-footer Commit-now button. **Set this true for agentic-mode driving** -- no user is at the keyboard to click the button. Hard cap: 2 nudges per turn; a 3rd would-fire aborts the turn with "Agent appears stuck (3 reasoning watchdog trips in one turn)." |

**The server launches with NO GUI window open.** It is waiting for you to
send a `launch` op. This lets you stage files (e.g. write a custom
`.locus/config.json`) before the GUI starts.

---

## Wire protocol

- One TCP connection per round trip is fine. So is one persistent
  connection per session. The server reads UTF-8 lines terminated by
  `\n`; each line is one request; the response is one line in return.
- Request:

    ```json
    {"op": "<op>", "args": {...}, "id": "<optional client tag>"}
    ```

- Response:

    ```json
    {"ok": true,  "detail": "...",                "data": {...}, "id": "..."}
    {"ok": false, "detail": "...", "error": "...", "data": {...}, "id": "..."}
    ```

  `data` is present when the op returns structured payload (e.g. text
  content, file contents, listings). `detail` is free-text. `id` is
  echoed back if you sent one.

### PowerShell helper -- ship-and-use, or inline-paste

Future-you will not have a persistent shell across `Bash` tool calls.
Two options for talking to the server from PowerShell:

**Option A (recommended) -- use the shipped helper.** The repo includes
`tests/ui_automation/locus-op.ps1`. From any cwd, invoke it by absolute
path. Two parameter sets:

```powershell
# Raw JSON request:
& "d:\Projects\AICodeAss\tests\ui_automation\locus-op.ps1" -Json '{"op":"ping"}'

# Op + args shorthand (script builds JSON via ConvertTo-Json):
& "d:\Projects\AICodeAss\tests\ui_automation\locus-op.ps1" `
    -Op "submit_chat" -Args @{ text = "write a snake game in C++" }

# Pipe straight into ConvertFrom-Json:
$resp = & "d:\Projects\AICodeAss\tests\ui_automation\locus-op.ps1" `
            -Op "read_chat" -Args @{ max_chars = 4000 } | ConvertFrom-Json
$resp.data.text
```

Exit codes: 0 = response returned (whether ok=true or false), 2 = socket
error / server not running, 3 = bad argument combo. The script is one
~80-line file with no external dependencies -- copy-deployable.

**Option B (legacy) -- inline preamble.** If you'd rather not depend on
the shipped helper, the same logic in one function:

```powershell
function locus-op([string]$Json, [int]$Port = 7878) {
    $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $Port)
    try {
        $s = $c.GetStream()
        $b = [Text.Encoding]::UTF8.GetBytes($Json + "`n")
        $s.Write($b, 0, $b.Length)
        $r = New-Object IO.StreamReader($s, [Text.Encoding]::UTF8)
        $r.ReadLine()
    } finally { $c.Close() }
}
locus-op '{"op":"ping"}'
```

For long arguments (file contents, multi-line prompts) compose the JSON
in PowerShell as an object then `ConvertTo-Json -Compress`:

```powershell
$req = @{ op = "write_workspace_file"; args = @{ path = "CMakeLists.txt"; content = "cmake_minimum_required(VERSION 3.20)`nproject(snake)`nadd_executable(snake snake.cpp)`n" } } | ConvertTo-Json -Compress
locus-op $req
```

`ConvertTo-Json` handles the escaping; do not hand-build long JSON
strings.

### Parsing responses

```powershell
$resp = locus-op '{"op":"read_chat"}' | ConvertFrom-Json
$resp.ok            # true / false
$resp.data.text     # chat content
$resp.data.length
$resp.detail
```

---

## Op catalog

Ops fall into three groups: existing UIA ops (lifted from the scripted
runner), agentic extensions, and server-handled meta ops.

### Server-handled meta ops

| Op | Args | Purpose |
|---|---|---|
| `ping` | -- | Round-trip check. Returns `detail: "pong"`. |
| `info` | -- | Returns `data: { workspace_dir, output_dir, locus_gui, port, step_index }`. |
| `shutdown` | -- | Closes the GUI and exits the server. |

### UIA primitives (same shape as scripted-runner ops)

| Op | Args | Notes |
|---|---|---|
| `launch` | `extra_args` (optional array of strings) | Spawn `locus_gui.exe`. Required before any UI op. |
| `wait_for_window` | `title_prefix` (default `"Locus"`), `timeout_ms` (default 15000) | Blocks until a top-level window of the launched process matches. Always call this after `launch`. |
| `find` | `automation_id` / `name` / `parent_aid` + `control_type`, `timeout_ms` | Probe that a control is reachable. |
| `click` | same as `find` | Invokes the control. |
| `type` | `automation_id`, `text`, `timeout_ms` | Sets the value of an Edit/Document control. |
| `press_key` | `key`, optional `modifiers`, optional target | Keys: `ENTER`, `ESC`, `TAB`, `SPACE`, `BACKSPACE`, arrows, `F1..F12`, etc. Target args route the key to a specific HWND via PostMessage; otherwise SendInput against the foreground. |
| `select_tab` | `parent_aid` + `index` *or* `name` | Index path is the reliable one for wxNotebook. |
| `get_text` | target, optional `walk_subtree` | Returns `data: { text, length }`. |
| `assert_text_contains` | target, `substring`, `timeout_ms`, `walk_subtree` | Polls. Use when waiting for known content. |
| `assert_visible` | target, `timeout_ms` | Checks the element isn't offscreen. |
| `screenshot` | optional `name` (default `step_NN.png`) | Saved to the output dir; returns `data: { path }`. |
| `sleep` | `ms` (default 100) | Use rarely. |
| `quit` | `wait_ms` (default 3000) | Closes the GUI; the server stays alive (you can `launch` again with different config). |
| `dump_tree` | optional `name`, `depth` (default 8) | Full UIA tree to a file; returns `data: { path, bytes }`. |
| `assert_file_exists` | `path` or `any_of[]`, `timeout_ms` | Workspace-relative paths. |
| `assert_file_contains` | `path`, `substring`, `timeout_ms` | Workspace-relative. |

Targeting reference (any op that takes a target):

- `automation_id`: matches the value of `wxWindow::SetName()` (see
  `src/frontends/gui/ui_names.h` for the full inventory).
- `name`: matches the accessible name (button label, etc.).
- `parent_aid` + `control_type`: find the parent by `automation_id`, then
  the first child of the given control type. Use `deep: true` to walk
  the whole subtree.

### Agentic extensions

| Op | Args | Returns / notes |
|---|---|---|
| `submit_chat` | `text` | Types into the chat input and presses Enter. Returns `data: { chars, dispatched, since_byte }` where `dispatched` is one of `"slash" \| "unknown_slash" \| "template" \| "agent" \| "unknown"`, derived from a 300ms log-tail. `unknown` means the dispatch class couldn't be classified quickly -- poll `get_chat_status` or `read_locus_log` for the eventual ground truth. |
| `read_chat` | optional `max_chars` (default 20000), `from_webview` (default true), `timeout_ms` | Returns `data: { text, truncated, length }`. Reads the chat WebView DOM via UIA. **Limitations to know about:** (a) long tool-call / write_file bubbles are collapsed behind a "Show N more lines" chevron the UIA scrape can't expand; (b) tool ERROR results render as `<name>  Error  Error  (Nt)` with the error body scrubbed regardless of size; (c) on turn cancel the in-flight assistant bubble is discarded -- a `read_chat` taken mid-stream will shrink to just the committed history on the next read. **Use `dump_history` instead** when you need the verbatim tool args / tool results -- it pulls from the persisted session JSON which carries everything. |
| `dump_history` | optional `session_id`, `max_messages` (default 0 = all), `include_system` (default false), `max_content_chars` (default 0 = no cap) | Returns `data: { messages: [...], session_id, session_file, present, full_message_count, estimated_tokens }`. Reads the most-recently-modified `.locus/sessions/*.json` (or the named one) and surfaces the verbatim message array with full `tool_calls` args + tool-result `content`. Closes the gaps in `read_chat`. NOTE: the session file reflects state through the most recent turn boundary -- an in-flight assistant message only appears once the turn completes (auto-save fires on `on_turn_complete`). |
| `slash` | `command` (string, with or without leading `/`), optional `wait_ms` (default 3000) | Types `/command ...` into the chat input and tails the log for the dispatch marker. Returns `data: { dispatched, command, name, log_marker, since_byte }` where `dispatched` is one of `"slash" \| "unknown_slash" \| "parse_error" \| "template" \| "agent" \| "unknown"`. Use this instead of `submit_chat` when you want a clean dispatch contract for slash testing. |
| `wait_for_text_stable` | target, `stable_ms` (default 3000), `timeout_ms` (default 120000), `poll_ms`, `walk_subtree` (default true), `min_chars` | Polls the target's text until it stops changing for `stable_ms`. Primary way to detect "agent finished streaming". Returns `data: { polls, length, elapsed_ms }`. |
| `wait_for_log_contains` | `substring`, `timeout_ms` (default 60000), `since_byte`, `poll_ms` | Polls `.locus/locus.log` for a substring. Returns `data: { matched_at_byte, log_size }`. Use `since_byte` from a previous `read_locus_log` to skip over already-seen output. |
| `list_workspace` | optional `path`, `depth` (default 3), `max_entries` (default 500), `include_hidden` (default false; `.locus/` is always included) | Recursively lists files under a workspace-relative path. Excludes build/node_modules/etc. Returns `data: { entries: [{path, is_dir, size?}], count, truncated }`. |
| `read_workspace_file` | `path`, optional `max_bytes` (default 200000) | Returns `data: { text, truncated, size, length }`. Refuses paths outside the workspace. |
| `write_workspace_file` | `path`, `content` | Creates parent dirs. Use for staging test inputs (e.g. a custom `.locus/config.json`) before launch. |
| `delete_workspace_file` | `path` | -- |
| `read_locus_log` | optional `tail_lines` (default 200), `since_byte` | Returns `data: { lines, size, present }`. Pass `since_byte` to get only new output since the last call. |
| `get_chat_status` | optional `timeout_ms` (default 1000) | Snapshots the chat-footer status. Returns `data: { context_label, plan_chip, commit_chip, compacted_chip, stop_btn_visible, action_label, agent_busy, cancel_in_progress }`. **`context_label`** is the rendered ctx meter text (e.g. `"6431 / 16384"`); empty when the widget is missing. `plan_chip` / `commit_chip` / `compacted_chip` are kept for back-compat with older agentic scripts -- footer-declutter retired the dedicated widgets, so they may return empty strings on current builds. **`agent_busy`** is the boolean callers usually want -- true when the action button reads "Stop" or "Queue", false when it reads "Submit". **`cancel_in_progress`** is true when the LAST `AgentCore: cancellation requested` log line has no matching `turn cancelled` / `LLM stream aborted` / `agent thread stopped` line after it -- the Stop button flips to "Submit" within ~4s but the cancel can take minutes to propagate through a streaming LLM call. Check this before queuing a follow-up `submit_chat` so the new prompt doesn't collide with a still-cancelling turn. |
| `list_named_widgets` | optional `depth` (default 12) | Walks the UIA tree and returns every line whose AutomationId / Name mentions `locus.`. Use when you need to discover what's addressable in a state you haven't seen before. Returns `data: { lines, count }`. Lines are formatted as `[ControlType] aid="..." wx_id="..."` -- **the `aid="..."` field is what you pass as `automation_id` in `find` / `click`** (matches the UIA Name property because LocusAccessible::GetName surfaces SetName there). `wx_id` is the internal wxWidgets numeric id; NOT addressable. |
| `wait_for_agent_idle` | optional `timeout_ms` (default 600000 = 10 min), `poll_ms` (default 1000), `quiet_ms` (default 4000), `stuck_after_quiet_ms` (default 300000 = 5 min) | Combines log-mtime stability + action-button state to detect end-of-turn. Returns `{status, action_label, log_quiet_ms, elapsed_ms}` where `status` is `"idle"` (button = Submit AND log quiet for `quiet_ms`), `"stuck"` (button = Stop/Queue AND log silent for `stuck_after_quiet_ms` -- agent thread hung), or `"timeout"`. **Prefer this over `wait_for_text_stable parent_aid=locus.chat.webview`** -- DOM stability lies during long reasoning / tool-call rounds where nothing visible streams. The `stuck` false-positive rate is now low because the LLM stream emits an info-level chunk heartbeat every 32 chunks / 10s (see "What agentic mode turns on automatically" above) -- a 5-minute log silence really does indicate a frozen agent thread, not just a deep reasoning round. |

---

## Lifecycle of a session

1. Launch the server. Note the port (echoed to stdout; also in
   `agentic.port`).
2. (Optional) `write_workspace_file` any staged inputs -- e.g. a custom
   `.locus/config.json` to pick a model, a `LOCUS.md` to give Locus
   workspace context, seed source files for the scenario.
3. `launch` -> `wait_for_window`. The GUI window appears.
4. Drive the scenario. Typical loop:
    - `submit_chat` a prompt.
    - `wait_for_agent_idle` -- waits for the chat action button to flip back to "Submit" AND the workspace log to go quiet (4s default). Returns `status: "stuck"` if the agent thread froze (busy button, silent log) so the caller can bail without burning the full timeout. Replaces the older `wait_for_text_stable` recipe -- DOM stability lies on long reasoning rounds.
    - `read_chat` to get the latest content.
    - `read_locus_log since_byte=<previous>` for the diagnostic trail.
    - `list_workspace` / `read_workspace_file` to verify what got written.
    - `screenshot` whenever you want a visual artifact for the report.
    - React: if the agent went off track, send a corrective message, try
      a different prompt phrasing, swap models via Settings, etc.
5. When done with one run: `quit` (GUI closes, server stays up). You can
   then change config (`write_workspace_file`) and `launch` again, or
   `shutdown` to exit.

---

## Recipe -- "test how Locus creates a small C++ minigame"

This is the canonical scenario. Goal: see how the local agent handles
a multi-step build task with a small model. Report what worked, what
broke, what could be improved.

### Setup

```powershell
# 1) Make sure LM Studio is loaded with the model under test.
# 2) Start the server in one shell, leave it running:
build\release\tests\ui_automation\Release\locus_ui_tests.exe `
    --agentic --workspace tmp --port 7878
# Note the workspace dir it prints -- you'll reference files there.

# 3) From another shell, smoke the protocol:
function locus-op([string]$Json,[int]$Port=7878) {
    $c=New-Object Net.Sockets.TcpClient('127.0.0.1',$Port);
    try { $s=$c.GetStream();
          $b=[Text.Encoding]::UTF8.GetBytes($Json+"`n");
          $s.Write($b,0,$b.Length);
          $r=New-Object IO.StreamReader($s,[Text.Encoding]::UTF8);
          $r.ReadLine() } finally { $c.Close() }
}
locus-op '{"op":"ping"}'                            # expect "pong"
locus-op '{"op":"info"}' | ConvertFrom-Json
```

### Drive the scenario

```powershell
# Launch GUI
locus-op '{"op":"launch"}'                          # spawns locus_gui.exe
locus-op '{"op":"wait_for_window"}'                 # blocks until window is up

# Initial chat prompt
$prompt = "Create a small terminal-based Snake game in C++ as `snake.cpp`. " +
          "Add a CMakeLists.txt that builds it as `snake.exe`. " +
          "Use the Windows console API for input; keep the code under 200 lines."
$req = @{ op="submit_chat"; args=@{ text=$prompt } } | ConvertTo-Json -Compress
locus-op $req

# Wait for the agent to finish.
# Reads the chat action-button state ("Submit" = idle) AND the .locus/locus.log
# mtime; returns status "idle" / "stuck" / "timeout". Prefer this over the
# older DOM-stability recipe -- a long reasoning round can hold the chat DOM
# still for minutes while the agent is hard at work.
$wait = @{ op="wait_for_agent_idle";
           args=@{ timeout_ms=600000; quiet_ms=4000;
                   stuck_after_quiet_ms=300000 } } | ConvertTo-Json -Compress
locus-op $wait

# Snapshot state. For tool args / tool results, prefer dump_history
# over read_chat -- read_chat collapses long write_file bubbles behind
# "Show N more lines" chevrons and scrubs error bodies regardless of
# size. dump_history pulls the verbatim message array from the saved
# session JSON.
locus-op '{"op":"screenshot","args":{"name":"after_first_turn.png"}}'
$history = (locus-op '{"op":"dump_history"}' | ConvertFrom-Json).data
$history.messages.Count                                    # all messages
$history.messages | Where-Object { $_.role -eq "tool" }    # tool results
$chat = (locus-op '{"op":"read_chat","args":{"max_chars":10000}}' | ConvertFrom-Json).data.text
$files = (locus-op '{"op":"list_workspace"}' | ConvertFrom-Json).data.entries
$log   = (locus-op '{"op":"read_locus_log","args":{"tail_lines":300}}' | ConvertFrom-Json).data
```

### React to what you see

A small model will probably do *something* wrong. Examples and how to
respond:

- **The agent stopped halfway through.** Send a follow-up: "continue
  with the CMakeLists.txt". Note the failure in your findings.
- **The agent wrote `snake.cpp` but didn't compile-check.** Ask it to
  `run_command "cmake -B build && cmake --build build"`. If it refuses
  or generates broken syntax, note the model behaviour.
- **The agent looped on a tool call.** Check `read_locus_log` for the
  same tool name appearing many times; note that pattern.
- **The agent rejected a file write because it was an `ask` policy.**
  Either pre-seed `.locus/config.json` with `"auto"`, or click Approve
  in the dialog. (Approval dialog automation: `find name="..."` then
  `click automation_id="locus.tool_approval.approve_btn"`.)

Each interesting deviation is a *finding*. Capture:

- Short description (what the agent did vs. what was asked).
- Quoted chat excerpt or log line.
- A screenshot if visual.
- A guess at the root cause: model limitation, prompt template issue,
  tool-call schema confusion, tool-approval friction, etc.

### Iterate

After each run, you can:

- `quit`, then `write_workspace_file '.locus/config.json' '...'` with a
  different model preset / temperature / tool format, then `launch`
  again. Compare behaviour.
- Or `shutdown` and restart the server with `--workspace tmp` for a
  fully fresh state.

### Write up findings

End the session by writing a markdown report. Recommended location:
`tests/ui_automation/output/agentic_<wslabel>/findings.md` (next to
`agentic.log`). Structure:

```markdown
# Agentic test report -- <date>

## Setup
- Workspace: <path>
- Model: <name / quant / context>
- Locus commit: <git rev-parse HEAD>
- Prompt: <verbatim>

## Findings
1. **<short title>** -- <one-line description>
   - Evidence: <chat excerpt | log line | screenshot path>
   - Likely cause: <hypothesis>
   - Suggested fix: <prompt change | code path | telemetry to add>

2. ...

## What worked
- <thing>

## Suggested follow-ups
- <thing>
```

The user reviews this, you fix things in a follow-up session, then
re-run the scenario to confirm.

---

## Recipe -- "small LLM tool-calling robustness"

Same shape, different focus. Switch model between runs (via
`write_workspace_file '.locus/config.json'` + `quit` + `launch`),
prompt with tasks of increasing tool-call complexity, look for:

- Malformed tool-call XML / JSON (check `.locus/locus.log` for
  `dropped tool call` or `JSON parse` warnings).
- Hallucinated tool names (check the chat for `Unknown tool 'X'`
  responses).
- The agent giving up vs. retrying after a tool error.
- Tool-call format drift mid-stream (decoder warnings).

The relevant settings to vary live on the Settings -> LLM tab:
`tool_format` dropdown (`auto`, `openai_json`, `qwen_xml`, `claude_xml`)
and the model presets row (S4.V Task 6). Toggle via UIA:

```powershell
locus-op '{"op":"click","args":{"automation_id":"locus.main_frame"}}'   # ensure focus
locus-op '{"op":"press_key","args":{"key":"COMMA","modifiers":"CTRL"}}' # Ctrl+, opens Settings
locus-op '{"op":"select_tab","args":{"parent_aid":"locus.settings.notebook","index":0}}'
# ... select preset, OK
```

Tedious to script; usually easier to `write_workspace_file
'.locus/config.json'` directly between launches.

---

## Writing prompts for slow local models

The canonical "create a small C++ minigame" recipe assumes a model fast
enough to keep tool-call latency under ~30s per round. Reasoning-heavy
models at 16k context (Qwen 3.x 27B, Gemma-3 reasoning variants, GPT-OSS)
spend 5-25 min in reasoning per round and run a real risk of either
exhausting context or never emitting a tool call before the QA-LLM gives
up. The TestLocalVibe session (Qwen 3.6 27B, May 2026) caught this: a
wide-scope edit prompt produced 25 minutes of reasoning and zero tool
calls, while a tight-scope retry of the same edit landed in 3 minutes.

When you must use a slow / reasoning-heavy local model:

- **Disable reasoning where the model supports it.** Qwen's `/no_think`
  suffix is the canonical example; append it to the prompt and per-round
  latency drops by an order of magnitude.
- **Forbid plan-making explicitly.** "Do not produce a plan. One-sentence
  acknowledgement, then tool calls." cuts the per-round reasoning bubble
  from minutes to ~20s.
- **Provide exact line anchors / file paths.** Don't make the model
  search for what to edit -- it'll re-read three files before deciding.
  "Edit src/foo.cpp:42, change `static const int X` to `static int X`."
- **Pick the tool upfront.** "Use `edit_file` (not `write_file`)." saves
  a round where the model is deciding which to call.
- **Cap scope.** A single one-line change is reliably round-tripable;
  a "refactor X into multiple files" prompt is not.

For agentic test runs that need to verify FUNCTIONALITY rather than
PROBE model behaviour, swap to a smaller / faster model for the run --
Gemma 4 E4B and Qwen 3.5 9B both round-trip a "create snake.cpp + cmake
build" scenario in under 10 minutes. Reserve the 27B+ models for sessions
where the slow / reasoning behaviour is itself what you're testing.

---

## Capturing findings -- what to save

The output dir (`<output_root>/agentic_<wslabel>/`) is your scratchpad
and report destination. By the end of a session it should contain:

- `agentic.log` -- written by the server, one line per op. Don't edit.
- `agentic.port` -- the port number. Stale after server exits.
- Screenshots you took (`step_NN.png` defaults, or whatever you named).
- `tree.txt` or other `dump_tree` outputs.
- **`findings.md`** -- the report. You write this at the end.
- Optional: copy the workspace's `.locus/locus.log` and any generated
  source files into the output dir so the report's references are
  self-contained.

Do NOT delete the tmp workspace dir until you've copied what you need.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `locus-op` throws "No connection could be made" | Server isn't running, or port mismatch. Read `agentic.port` from the output dir. |
| `launch` returns ok but `wait_for_window` times out | Another Locus instance holds the workspace lock. Check `.locus/locus.lock` -- only one process per workspace at a time. |
| Chat input lookup by `locus.chat.input` fails | The chat input is a wxTE_RICH2 control whose native RichEdit `IAccessible` overrides our shim. `submit_chat` already handles the fallback; if you're doing a manual `type`, target `parent_aid="locus.chat.panel"` + `control_type="document"`. |
| `find` on a single-line wxTextCtrl inside an initially-hidden AUI pane (`locus.memory_bank.search` was the canonical case) returns timeout | wxTextCtrl with `wxTE_PROCESS_ENTER` style created under a hidden parent doesn't surface in the UIA tree at all, even after the parent is toggled visible -- not even via `parent_aid` + `control_type=edit` + `deep=true`. The adjacent wxStaticText label IS in the tree; the EDIT widget itself is absent. Same widget without `wxTE_PROCESS_ENTER` (e.g. the `wxTE_MULTILINE` detail editor) surfaces fine. Investigated 2026-05-27; root cause is internal to wxWidgets-MSW's MSAA bridge for this style. **Workaround**: skip the affected widget in the script (use `dump_tree` to confirm what's actually findable under the parent, then assert only on those names); if you must verify the search input separately, drive it via keyboard focus into the panel + `press_key` rather than UIA-finding the widget. Tracked in [S6.17](../../roadmap/M6/S6.17-compaction-fixes-and-tool-robustness.md) Task K as a wxWidgets-side investigation, not blocking. |
| Memory Bank panel (or another initially-hidden AUI pane) is visible at baseline of a `tmp`-workspace script run | Locus persists the wxAUI perspective in **global** `<global_dir>/ui_state.json` (e.g. `~/.locus/ui_state.json`), not per-workspace. A previous failed UIA run that exited with the panel shown pollutes every subsequent launch -- fresh `tmp` workspaces inherit the layout. Workaround: delete `~/.locus/ui_state.json` between runs, or back it up at the start of a debug session and restore at the end. Permanent fix tracked in [S6.17](../../roadmap/M6/S6.17-compaction-fixes-and-tool-robustness.md) Task K (move `aui_perspective` to per-workspace `.locus/ui_state.json`). |
| `wait_for_text_stable` returns success too early | The agent paused for a long tool call (e.g. a slow shell command) longer than `stable_ms`. Raise `stable_ms` to 8000+ for scenarios that include long-running tool calls, OR switch to `wait_for_agent_idle` which combines log-mtime + action-button state rather than DOM stability. |
| `wait_for_agent_idle` returns `status: "stuck"` | Two distinct shapes: (a) the agent thread froze mid-tool (the canonical case is the `run_command` ReadFile hang documented in `output/agentic_Tetris/findings.md`) -- check `.locus/locus.log` for `run_command[pid=N]: reader still draining after ...` and set `agent.tool_max_runtime_s` as a workaround; (b) the agent is reasoning silently for minutes without emitting tool calls (TestLocalVibe Qwen 27B Task 2) -- set `agent.reasoning_max_chars` + `agent.reasoning_auto_nudge=true` and the watchdog will cancel + nudge automatically instead of letting the round run to the `llm.timeout_ms` ceiling. |
| `read_chat` truncates important content | Raise `max_chars`; the cap is just to keep payloads bounded. |
| Tool approvals stop the agent | Either pre-seed `.locus/config.json` with `"auto"` for the relevant tool, or watch for `locus.tool_approval.dialog` and click `locus.tool_approval.approve_btn`. The default config seeded for `tmp` workspaces auto-approves write/edit/delete/run_command. |
| The screen is busy with other windows | UIA captures the launched Locus window specifically; other windows on the desktop don't matter. But focus theft is real -- the safer pattern is to run on a session you control or in a secondary Windows session. |
| Server exits unexpectedly | Check `agentic.log` for the final lines. The GUI process going away does NOT shut down the server -- you must send `shutdown` explicitly. |
| `find` / `wait_for_window` / `read_chat` returns `error: "<op>: launched gui process (pid=N) has exited (exit_code=K) ..."` | The `locus_gui.exe` child crashed or exited after `launch`. The harness now fails fast with the pid + exit code instead of letting the find timeout elapse and reporting a misleading "timeout waiting for element". Check `.locus/locus.log` (last few lines before exit), `.locus/dumps/*.dmp` if `agent.dump_on_run_command_hang` was on, and the `agentic.log` op trail. Recover with `quit` followed by `launch` to spawn a fresh GUI; the server stays up across crashes. |
| Build errors | Make sure you built the `locus_ui_tests` target after pulling -- the new files (`op_dispatcher.cpp`, `agentic_server.cpp`) need to be in the binary. |

---

## What this isn't

- **Not a replacement for unit / integration tests.** Those are fast,
  deterministic, run automatically. Agentic mode is for the messy
  things they can't catch.
- **Not a load test.** Single client, single GUI, sequential ops.
- **Not a security test.** The TCP port is `127.0.0.1`-bound by default
  for a reason; don't expose it on the public network.
- **Not for unsupervised CI.** UIA steals focus and the LLM-driven
  client makes non-deterministic choices. Useful for "scheduled
  exploratory runs", not for "block-merge-on-fail".

---

## Pointers

- Op handlers: [op_dispatcher.cpp](op_dispatcher.cpp)
- TCP server: [agentic_server.cpp](agentic_server.cpp)
- Named widget catalog: [../../src/frontends/gui/ui_names.h](../../src/frontends/gui/ui_names.h)
- Scripted UIA reference (sister doc): [README.md](README.md)
- Manual test plans (complement to UIA + agentic): [../manual/README.md](../manual/README.md)
