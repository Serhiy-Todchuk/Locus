# Locus UI Automation Tests (S5.L)

A scripted Windows UI Automation (UIA) driver for `locus_gui.exe`. Drives the
real wx event path -- click a button, type into a control, screenshot the
window, assert state -- so smoke-level GUI regressions become catchable from
a subprocess.

Unlike `locus_tests` and `locus_integration_tests`, this suite:

- **Pops real windows on the desktop session.** Steals focus, moves the mouse,
  types keystrokes. Don't run it while you're trying to do something else --
  use a separate Windows session (`runas /user:locustest`), an RDP loopback,
  or a dedicated CI runner.
- **Manual only.** Not registered with `ctest` / `catch_discover_tests`. Same
  protocol as `tests/integration/`: run explicitly when needed.
- **Windows only.** UIA is a Windows API. Cross-platform UI testing is a
  separate problem -- AT-SPI on Linux, `AXUIElement` on macOS.
- **Many scripts ship now.** The initial S5.L set was three (smoke /
  settings-tour / chat-round-trip); the post-S5.L expansion adds capability
  modal coverage, settings persistence + sub-controls, popups (slash, MCP
  open-json, compaction), mutating-tool friction confirm, plus LLM-driven
  tests for the approval dialog, inline diffs, and the terminal panel.

## What ships

```
tests/ui_automation/
  main.cpp              # entry: parses CLI, dispatches to ScriptRunner
  uia_session.{h,cpp}   # COM wrapper: find / click / type / screenshot
  script_runner.{h,cpp} # JSON-driven step dispatcher
  scripts/              # each .json = one test
    smoke.json
    settings_tour.json
    chat_round_trip.json
    plan_mode.json                  # LLM-dependent
    capabilities_first_open.json
    menu_and_view_toggles.json
    settings_persistence.json
    settings_preset_dropdown.json
    save_as_global_defaults.json
    mcp_open_json.json
    slash_popup.json
    help_slash.json
    compaction_dialog.json
    mutating_tool_friction.json
    tool_approval_dialog.json       # LLM-dependent
    inline_diff_render.json         # LLM-dependent
    terminal_panel_run.json         # LLM-dependent
    terminal_per_tab.json           # S5.R, LLM-dependent
    system_prompt_bubble.json       # S5.G
  output/               # gitignored: per-script artifacts land here
```

## Build

```
cmake --build build/release --config Release --target locus_ui_tests
```

Output: `build/release/tests/ui_automation/Release/locus_ui_tests.exe`. The
GUI target (`locus_gui`) is a build-dependency of the test exe -- building
the tests implicitly rebuilds the GUI if needed.

## Run

### One script

```
build\release\tests\ui_automation\Release\locus_ui_tests.exe ^
    --script tests\ui_automation\scripts\smoke.json
```

### All scripts in the bundled `scripts/` folder

```
build\release\tests\ui_automation\Release\locus_ui_tests.exe --all
```

### List

```
build\release\tests\ui_automation\Release\locus_ui_tests.exe --list
```

### Other flags

| Flag | Effect |
|---|---|
| `--gui <path>`         | Override `locus_gui.exe` location. Default: baked into the binary at build time. |
| `--output-root <dir>`  | Where per-script artifacts land. Default: `tests/ui_automation/output/`. |
| `--scripts-dir <dir>`  | Where `--all` looks for scripts. Default: bundled `scripts/` folder. |

### Exit codes

| Code | Meaning |
|---|---|
| 0  | every requested script passed |
| 1  | at least one script failed |
| 2  | bad usage / configuration error (missing `locus_gui.exe`, etc.) |
| 77 | all scripts skipped (no script flag, no `--all`) -- matches `tests/integration/` convention |

## Artifacts per run

```
output/<script_name>/
  run.log              # ordered step record + final PASS/FAIL line
  <step>.png           # screenshot, named by `op: screenshot` `name` arg or step index
  ...
output/<script_name>.xml  # JUnit-style report (one <testcase> per step)
```

## Step ops

Each step in a script is `{ "op": "<name>", "args": { ... } }`. Unknown ops
fail the script loudly.

| Op | Args | Notes |
|---|---|---|
| `launch`               | `extra_args` (optional array of strings) | Spawn `locus_gui.exe` against the script's workspace. |
| `wait_for_window`      | `title_prefix` (default `"Locus"`), `timeout_ms` (default `15000`) | Blocks until a visible top-level window of the launched process matches. |
| `find`                 | `automation_id` *or* `name`, `timeout_ms` | Sanity-check that a control is reachable. |
| `click`                | `automation_id` *or* `name`, `timeout_ms` | Prefers `InvokePattern`, falls back to `SelectionItem`, `Toggle`, then synthesised mouse click. |
| `type`                 | `automation_id`, `text`, `timeout_ms` | Prefers `ValuePattern.SetValue` (atomic); falls back to per-character `SendInput`. |
| `press_key`            | `key`, optional `modifiers` (`"CTRL"`, `"ALT"`, `"SHIFT"`, plus/comma separated) | Keys: `ENTER`/`RETURN`, `ESC`/`ESCAPE`, `TAB`, `SPACE`, `BACKSPACE`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `COMMA`, `PERIOD`, `BACKTICK`/`TILDE`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `DELETE`, `F1`..`F12`. |
| `select_tab`           | `parent_aid` + `index` (zero-based), or `name` for label-bearing tabs | wxNotebook on Windows wraps SysTabControl32 which doesn't propagate tab labels to UIA Name -- so `parent_aid` + `index` is the reliable shape. The `name` shape works for wxAuiNotebook and other strips that DO surface labels. |
| `get_text`             | `automation_id`, optional `walk_subtree` | Reads a control's value (Edit / Text / Document patterns). |
| `assert_text_contains` | `automation_id`, `substring`, optional `walk_subtree`, `timeout_ms` | Polls until the substring appears or the timeout elapses. |
| `assert_visible`       | `automation_id`, `timeout_ms` | Element exists and isn't off-screen. |
| `screenshot`           | optional `name` (default `step_NN.png`) | `PrintWindow(PW_RENDERFULLCONTENT)` -- captures WebView2 content. |
| `sleep`                | `ms` (default 100) | Use sparingly -- prefer `assert_text_contains` for real waits. |
| `quit`                 | `wait_ms` (default 3000) | WM_CLOSE first, terminate on timeout. Calling `launch` again later in the script is allowed -- the runner re-spawns against the same workspace dir, so settings-persistence-across-restart tests work in one script. |
| `dump_tree`            | optional `name` (default `tree.txt`), `depth` (default 8) | Walk the UIA tree under the launched window; useful for debugging an `automation_id` that doesn't resolve. |
| `assert_file_exists`   | `path` (workspace-relative) **or** `any_of` (array of relative paths), `timeout_ms` | Polls until at least one candidate exists. Used by tests that drive the LLM into writing a file or by Save-as-global-defaults to check `~/.locus/config.json` (paths are relative to the resolved workspace dir, so combine with `setup.env` and `{workspace}` substitution for paths outside it). |

## Naming controls

The driver looks up controls by `automation_id`, which maps to
`wxWindow::SetName()`. The single source of truth for these names is
[`src/frontends/gui/ui_names.h`](../../src/frontends/gui/ui_names.h) -- if a
new script needs to address a widget that doesn't yet have a name, add one
there and `SetName()` it in the corresponding `*_panel.cpp` / `*_dialog.cpp`.

The convention: `locus.<panel>.<widget>` (dot-segment naming, prefix keeps us
out of wx's own internal namespace).

**Important**: wxWidgets doesn't surface `SetName()` to UIA by itself --
controls need `gui::apply_locus_accessible_name(window)` from
[`locus_accessible.h`](../../src/frontends/gui/locus_accessible.h) right after
the `SetName()` call. That shim installs a tiny `wxAccessible` subclass that
reports the window's name through `IAccessible::get_accName`, which Windows
MSAA / UIA then expose as the `Name` property. Without it, panels surface
with empty Name and lookup falls back to control type only.

The shim works for ordinary wxWindows (panels, buttons, labels, toggles,
tabs, web views). It does **not** work for native Win32 controls that
provide their own `IAccessible` interface -- most importantly the
RichEdit-backed `wxTextCtrl(wxTE_RICH2)` in the chat input, which Windows
labels `"RichEdit Control"` regardless of what we set. For those, look up
by control type within a known parent (see the chat round-trip script).

## What UIA can and can't see

| Surface | Coverage |
|---|---|
| `wxFrame`, `wxPanel`, `wxButton`, `wxTextCtrl`, `wxStaticText` | done full |
| `wxToggleButton` (mode switcher) | done -- exposes `TogglePattern`; the driver maps `click` to `Toggle` |
| `wxNotebook` tabs (Activity panel, Settings dialog) | done -- tabs are `TabItem` controls; `select_tab` uses `SelectionItemPattern` |
| `wxWebView` (Edge backend) | done -- DOM exposed as a UIA `Document` sub-tree; chat content readable via `walk_subtree` |
| `wxTreeCtrl` (file tree) | done -- standard `Tree` / `TreeItem` |
| `wxTextCtrl` with `wxTE_RICH2` (chat input) | done but Windows reports its Name as `"RichEdit Control"` (native RichEdit overrides wxAccessibility). Address by control type `document` under the chat panel: `{ parent_aid: "locus.chat.panel", control_type: "document" }`. |
| `wxStyledTextCtrl` (Scintilla, tool approval JSON pane, terminal panel tabs) | x limited -- the wrapper is reachable by automation_id (e.g. `locus.tool_approval.args_view`) but the rendered text content doesn't propagate cleanly through UIA. Surface assertions (visibility, parent presence, tab existence) work; reading the buffer text doesn't. Use `SendMessage`/`SCI_GETTEXT` direct to the HWND when you really need the content. |
| `wxAuiNotebook` (main frame panels) | done exposes `TabItem`; click via `select_tab` |
| `wxPopupTransientWindow` (slash + @-mention popups) | done -- now named (`locus.chat.slash_popup` / `locus.chat.mention_popup`) and addressable via `assert_text_contains` with `walk_subtree`. |

## What we deliberately don't test via UIA

A few areas are excluded by design rather than by oversight:

- **Reading Scintilla content** -- the tool-approval JSON pane and terminal-panel tabs render with `wxStyledTextCtrl`. UIA exposes them as panes but not their rendered text. We assert surfaces (the dialog opens, the panel is visible, a specific button exists) and use the chat WebView as the parallel signal when the same content is mirrored there.
- **Right-click context menus** -- the harness has no `right_click` op today; the file-tree "Attach to context" path and terminal-panel tab context menus aren't UIA-tested. The single-click and Ctrl+key paths cover the deterministic surfaces.
- **Default-app handoff** -- `Open mcp.json` and `Open Global Config...` end with `wxLaunchDefaultApplication`. We assert the on-disk side effect (the file gets created), not that Notepad ended up open.
- **@-mention popup contents** -- the mention popup itself is named, but its candidate list comes from `IndexQuery::list_directory`, which is empty in a fresh tmp workspace. Tests for the popup will land once the harness gains a `setup.seed_files` option (or the test points at a non-tmp workspace).

## Setup options

The script root supports a `setup` block:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `workspace`                 | string | `"tmp"` | Workspace path. `"tmp"` creates `%TEMP%/locus_ui_tests/<script>_<rand>/` afresh each run. Any absolute path opens an existing workspace (script doesn't clean it up). |
| `allow_first_time_prompts`  | bool   | `false` | When true, skip the `.locus/config.json` pre-seed AND don't auto-inject `--no-first-time-prompts` to the launch args. Used by `capabilities_first_open.json` to exercise the Capabilities first-open modal. |
| `env`                       | object | `{}`    | Map of env-var to value, applied via `SetEnvironmentVariableA` in the harness before launch. `{workspace}` substitution is supported in values. Used by `save_as_global_defaults.json` to redirect `LOCUS_GLOBAL_DIR` into the script's own tmp dir. |
| `tool_approvals_override`   | object | `{}`    | Merges into the tmp workspace's pre-seeded `tool_approvals` block (tool name -> `"ask"`/`"auto"`/`"deny"`). Used by `tool_approval_dialog.json` to flip a tool that's auto-approved by default back to `ask` so the approval dialog actually fires. |
| `capabilities_override`     | object | `{}`    | Merges into the tmp workspace's pre-seeded `capabilities` block (capability key -> bool). Used by `terminal_stdin.json` to enable `background_processes` for the run since `--no-first-time-prompts` defaults them off. |
| `sessions_config`           | object | `{}`    | Merges into the tmp workspace's pre-seeded `sessions` block (`auto_cleanup_enabled`, `keep_last_count`, `delete_after_days`, `restore_last`, `persist_activity`). Override `restore_last: true` when seeding sessions you want auto-opened (the harness defaults it to `false` for determinism). Used by `manage_sessions_open.json` and `session_restore_round_trip.json`. |
| `sessions_seed`             | array  | `[]`    | Pre-populates `.locus/sessions/<id>.json` files before launch. Per-entry keys: `id`, `title`, `last_opened_at` (epoch seconds; negative = "ago" offset from now), `first_user_message`, `estimated_tokens`, optional `messages` (verbatim ChatMessage array -- use for full save/restore tests with reasoning + tool calls + tool results), optional `extras` (top-level merge -- e.g. `agent_mode`, `plan`). When `messages` is omitted the session lands as an empty stub matching the pre-S6.15 behaviour. |
| `ui_state`                  | object | `{}`    | When set, written verbatim to `.locus/ui_state.json`. Shape matches `WorkspaceUiState`: `{ "open_tabs": [{ "session_id", "title", "active" }] }`. Combined with `sessions_seed` + `sessions_config.restore_last: true` to auto-open a known set of tabs on launch. |

## Bundled scripts

| Script | Needs LLM? | What it covers |
|---|---|---|
| [`smoke.json`](scripts/smoke.json)                                 | no  | Launch, main window appears, three named panels reachable, screenshot, clean quit. |
| [`settings_tour.json`](scripts/settings_tour.json)                 | no  | Ctrl+, opens Settings, click through the 5 tabs, screenshot each, Esc closes the dialog, quit. |
| [`chat_round_trip.json`](scripts/chat_round_trip.json)             | no  | Type into chat input, press Enter, assert the user bubble appears in the WebView. User-side render only -- no agent reply asserted. |
| [`menu_and_view_toggles.json`](scripts/menu_and_view_toggles.json) | no  | `Ctrl+`` toggles the terminal pane. Validates the accelerator wiring + AUI pane show. |
| [`memory_bank_panel.json`](scripts/memory_bank_panel.json)         | no  | S5.K -- Ctrl+M toggles the View > Memory Bank pane. Verifies the panel + toolbar widgets (search, source/tag dropdowns, pinned-only / show-deleted checkboxes, list, bulk-op buttons) are addressable through UIA once shown. |
| [`chat_find_bar.json`](scripts/chat_find_bar.json)                 | no  | S5.Z task 2 -- Ctrl+F + chat-footer Find button both reveal the find bar (input + counter + Prev/Next + Aa toggle + close X). Asserts every named widget is reachable once shown, types a query, closes via Esc, reopens via the footer button, closes via the X. |
| [`capabilities_first_open.json`](scripts/capabilities_first_open.json) | no | Fresh workspace with `allow_first_time_prompts=true`; the S5.A capabilities modal appears, toggle a checkbox, OK, relaunch, verify the change persisted on the Capabilities tab. |
| [`settings_persistence.json`](scripts/settings_persistence.json)   | no  | Change temperature on the LLM tab, OK, quit, relaunch, verify the new value sticks. End-to-end Settings -> WorkspaceConfig -> config.json -> WorkspaceConfig round-trip. |
| [`settings_preset_dropdown.json`](scripts/settings_preset_dropdown.json) | no | Pick the `LM Studio + Qwen` preset, click `Load preset`, assert the Tool-call format dropdown updates to `Qwen`. Validates S4.V Task 6 preset wiring. |
| [`sampler_reset_and_auto_detect.json`](scripts/sampler_reset_and_auto_detect.json) | no | S6.10 Tasks H + F -- assert the new `locus.settings.llm.sampler_reset_btn` and `locus.settings.llm.auto_detect_preset` controls are reachable + clickable on the LLM tab. Loads a preset, clicks the sampler-reset button, toggles the auto-detect checkbox. UIA visibility is the deepest assertion (verifying values would need wxSpinCtrlDouble reads, which UIA can't do reliably). |
| [`permission_presets_settings.json`](scripts/permission_presets_settings.json) | no | S5.S -- Settings -> Tool Approvals, addresses the new preset `wxChoice`, snaps the highlight to the first row (Read-only). Validates the preset row is reachable from UIA. |
| [`permission_presets_chat_footer.json`](scripts/permission_presets_chat_footer.json) | no | S5.S -- assert the chat-footer permission preset chip + dropdown are findable by their `locus.chat.preset_*` automation ids. |
| [`save_as_global_defaults.json`](scripts/save_as_global_defaults.json) | no | Click `Save as global defaults` on the Settings dialog; with `LOCUS_GLOBAL_DIR` redirected via `setup.env`, asserts the resulting `config.json` lands on disk. Validates S5.M global-defaults write path. |
| [`mcp_open_json.json`](scripts/mcp_open_json.json)                 | no  | Settings -> MCP Servers -> Open mcp.json button. Asserts the stub `.locus/mcp.json` gets created. Doesn't assert the default-app launch (Notepad). |
| [`slash_popup.json`](scripts/slash_popup.json)                     | no  | Type `/sear` into the chat input; press Enter; assert the input now contains `/search` (the popup's Enter-handler accepted the highlighted suggestion). Observes the side effect on the input because the popup itself is a `wxPopupTransientWindow` whose UIA visibility is unreliable. |
| [`help_slash.json`](scripts/help_slash.json)                       | no  | Submit `/help`; assert the response appears in the chat WebView. Validates the deterministic slash-command path (no LLM round-trip). |
| [`compaction_dialog.json`](scripts/compaction_dialog.json)         | no  | Click the chat-footer Compact button; assert the CompactionDialog opens with both strategy radios, click Strategy C, Esc cancels. |
| [`mutating_tool_friction.json`](scripts/mutating_tool_friction.json) | no | Settings -> Tool Approvals; assert all five mutating-tool per-row wxChoice dropdowns are addressable (`locus.settings.approvals.choice.<tool>`). The S4.V Task 4 friction-confirm modal flow itself is harder to drive deterministically through UIA -- the manual test plan walks through the live behaviour. |
| [`plan_mode.json`](scripts/plan_mode.json)                         | yes | S4.D plan flow: switch to Plan, ask LLM to propose a plan, approve, verify a hello-world file lands on disk. Slowest script in the bundle (240s assertion timeouts). |
| [`tool_approval_dialog.json`](scripts/tool_approval_dialog.json)   | yes | Forces `write_file` to `ask` via `setup.tool_approvals_override`; LLM is asked to write a file; assert the ToolApprovalDialog opens, click Approve, assert the file appears. |
| [`inline_diff_render.json`](scripts/inline_diff_render.json)       | yes | LLM is asked to write a file (auto-approved); assert the resulting file content also appears in the chat WebView (S5.C inline-diff render). |
| [`terminal_panel_run.json`](scripts/terminal_panel_run.json)       | yes | LLM is asked to run a small shell command; assert the chat WebView shows the output, `Ctrl+`` brings up the terminal panel and the panel is visible. |
| [`terminal_per_tab.json`](scripts/terminal_per_tab.json)           | yes | S5.R -- terminal is per-tab. Spawn a `run_command` in tab A so the pane auto-shows, Ctrl+T to open tab B (empty terminal view), Ctrl+Shift+Tab back to A. Asserts the terminal panel stays addressable across tab switches. |
| [`terminal_stdin.json`](scripts/terminal_stdin.json)               | yes | S5.Z task 4 -- LLM is asked to start `more` via run_command_bg; assert the bg tab's stdin input (`locus.terminal.stdin_input.<id>`) is reachable once the terminal panel surfaces it. Uses `setup.capabilities_override.background_processes=true` so the bg capability is on under the unattended run. |
| [`system_prompt_bubble.json`](scripts/system_prompt_bubble.json)   | no  | S5.G -- assert the collapsed system-prompt bubble appears at the top of the chat WebView with its `System prompt (N tokens)` header. Per-section breakdown chips + the hover-reveal per-message X are inside a collapsed `<details>` element / driven by CSS hover; both stay in the manual test plan. |
| [`session_restore_round_trip.json`](scripts/session_restore_round_trip.json) | no | S6.15 -- seeds a fake session JSON (reasoning + tool_calls + tool result + final assistant answer) plus a matching `.locus/ui_state.json` so `restore_last` auto-opens it. Asserts the restored chat WebView shows every VISIBLE marker (user text, "Thinking", tool name, "Result" details summary, final answer). The bodies inside collapsed `<details>` blocks (the reasoning prose, tool-result body) aren't reachable from UIA, so `assert_file_contains` on the persisted session JSON covers them instead. Uses two new setup primitives -- `sessions_seed[].messages` (inline messages persisted verbatim) and `setup.ui_state` (writes `.locus/ui_state.json` verbatim). |

LLM-dependent scripts require LM Studio reachable at `http://127.0.0.1:1234` with a tool-calling model loaded. On flakes, see the per-script `output/<name>/run.log` and the workspace's `.locus/locus.log` for dropped-tool-call warnings (model behaviour, not the harness).

## Why no `--mock-llm` mode for chat tests in v1

Building a stub LLM is a separate subsystem with its own scoping
conversation. Until that exists, the chat-round-trip script asserts the
user-side of the round-trip only (user message renders into the chat view),
which doesn't need an LLM response. Assertions about agent replies stay in
[`tests/integration/`](../integration/README.md) where real model behaviour
is what's being checked anyway.

## Focus-stealing caveat

Running a UIA script pops windows on the developer's screen, steals focus,
and intercepts keystrokes. Three options:

1. **Use a secondary Windows session.** `runas /user:locustest` against a
   pinned-down account, or an RDP loopback session. The script runs in that
   session; your main desktop is undisturbed.
2. **Run from a CI runner.** GitHub Actions Windows runners have a desktop
   session out of the box. Not wired up yet -- documenting plausibility, not
   a recommendation to enable.
3. **Use only when you have time to wait.** Each script takes a few seconds;
   `smoke.json` is the fastest, `chat_round_trip.json` the slowest.

## Adding a new script

1. Write `scripts/my_feature.json` -- copy `smoke.json` for a no-frills
   starting point, `settings_persistence.json` if the feature persists
   across a relaunch, or one of the LLM-dependent scripts if a real model
   turn is needed.
2. If the steps need a widget that's not yet named, add an entry to
   [`src/frontends/gui/ui_names.h`](../../src/frontends/gui/ui_names.h) and
   wire it in the relevant `*_panel.cpp` / `*_dialog.cpp` with `SetName` +
   `gui::apply_locus_accessible_name`. Rebuild `locus_gui` (the test exe
   build will rebuild it for you).
3. Pick the right setup knobs:
   - First-open modal under test? `setup.allow_first_time_prompts: true`.
   - File outside the workspace? `setup.env: { "LOCUS_GLOBAL_DIR": "{workspace}/global" }`
     plus `assert_file_exists path: "global/config.json"`.
   - Approval gate under test? `setup.tool_approvals_override: { "write_file": "ask" }`.
4. Prefer addressing children over the dialog itself. Many wxDialogs surface
   their own `SetName` poorly through UIA; find a named button or checkbox
   inside instead. For owned dialogs (friction confirm, MessageBox), use
   `find name: "<exact title>"` rather than `wait_for_window` -- the latter
   filters owned windows.
5. Run the script: `--script scripts/my_feature.json`. Iterate until green.
   If a step needs debugging, add a `dump_tree` step before the failing one
   to see the UIA tree the harness saw.
6. Pass-rate matters. UIA timing is forgiving up to ~5 s on a fast machine;
   if your script flakes, audit the explicit waits (`assert_text_contains`,
   `assert_visible`, `assert_file_exists`) before reaching for `sleep` --
   explicit waits keep scripts fast on a quick machine and stable on a slow
   one.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Every script fails with `init: UIA COM init failed` | COM apartment conflict -- a parent process initialised STA differently. Run the exe standalone. |
| `find: timeout waiting for element (automation_id=...)` | Widget exists but no `SetName()` call -- check `ui_names.h` and the panel's create method. For dialogs the `SetName` on the dialog window itself often doesn't propagate (native window class wins); name a child control (a button, a checkbox) instead and find that. |
| `find` / `wait_for_window` returns `error: "<op>: launched gui process (pid=N) has exited (exit_code=K) ..."` | `locus_gui.exe` crashed or exited after `launch`. `UiaSession` now fails fast with the pid + exit code instead of letting the long find-timeout elapse (was an agentic Tetris finding). Check `.locus/locus.log` for the last lines before exit, `.locus/dumps/*.dmp` if `agent.dump_on_run_command_hang` was on; in scripts, this surfaces as a `FAIL` line with the message verbatim. |
| Settings dialog opens but tab clicks miss | The notebook AddPage label changed and the script's `name` arg drifted -- update the script to match the new label. |
| First few scripts pass, later ones fail at `find` / `press_key` after a back-to-back batch | Foreground was stolen by another process (the parent terminal) between launches. `wait_for_window` claims foreground via `AttachThreadInput + SetForegroundWindow + BringWindowToTop` for the launched window; if scripts still flake, the harness was likely run from a CMD/PowerShell that disagreed with Windows' foreground-lock policy. Run from the `--script` form rather than a batched `--all` if you're investigating one in isolation. |
| `wait_for_window` times out on an *owned* dialog (e.g. the friction confirm under Settings) | `wait_for_window` filters owned windows out (it walks unowned top-levels only). For owned dialogs use `find` with the dialog's exact title as `name=` -- `find` walks every visible top-level of the process regardless of owner. |
| A control is named but isn't in the UIA tree | Likely clipped: the wxDialog's fixed ctor size is smaller than its content, the bottom of the dialog falls below the client area, and the wxAccessible enumeration drops it. Add `outer->SetSizeHints(this);` after `Layout()` so the dialog fits its content (this caught the `cb_web` clipping in `CapabilitiesDialog`). |
| `wxChoice` selection-by-name doesn't fire `wxEVT_CHOICE` | `CBS_DROPDOWNLIST` doesn't expose its items in the UIA tree until the dropdown is expanded, and key navigation through PostMessage / SendInput doesn't reliably commit. Assert the dropdown is reachable; defer the modal-firing assertion to a manual test plan. |
| Screenshots show black rectangles for the chat WebView | Old PrintWindow without `PW_RENDERFULLCONTENT`. We pass that flag; if it ever fails, fall back to `BitBlt` after `RedrawWindow(RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW)`. |
| `screenshot: GetClientRect failed` mid-script after a modal closes | `target_hwnd_` is stale -- the modal's HWND was the most recent thing `wait_for_window` saw, and it's now destroyed. Add a fresh `wait_for_window` for the main `Locus` window after the modal closes; that re-binds `target_hwnd_`. |
| Process leaks between scripts | The harness sends `WM_CLOSE` then `TerminateProcess` on timeout. Each script gets its own workspace dir under `%TEMP%/locus_ui_tests/` so they don't compete for the WorkspaceLock. |
| `chat_round_trip` fails to find the user message | The WebView contents render async; `assert_text_contains` polls with a 10 s timeout. If your machine is loaded, raise the timeout to 30 s for that step. |

## Relationship to the other test suites

- **Unit tests (`locus_tests`)** -- fast, hermetic, no GUI. Untouched by S5.L.
- **Integration tests (`locus_integration_tests`)** -- live LLM, drives
  `AgentCore` end-to-end, no GUI. Untouched by S5.L.
- **UI automation tests (this)** -- drives the real wx widgets; most scripts
  are deterministic, four are LLM-dependent (require LM Studio) and document
  that in their `description`. The post-S5.L expansion adds coverage for the
  Capabilities modal, settings persistence + sub-controls, popups, the
  approval gate, inline diffs, and the terminal panel.
- **Manual tests (`tests/manual/`)** -- click-by-click QA plans that cover
  what UIA structurally can't (drag-and-drop visuals, focus order, file
  dialogs, third-party software workflows, Scintilla content, right-click
  context menus). UIA assertions and manual plans are complementary -- when
  a feature lands a manual plan AND a UIA script, the script asserts the
  surface presence, the plan asserts the behaviour the script can't see.
