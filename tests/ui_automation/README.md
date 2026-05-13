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
- **Three scripts ship in v1.** This proves the harness; broader coverage
  follows feature work in later M5 stages.

## What ships

```
tests/ui_automation/
  main.cpp              # entry: parses CLI, dispatches to ScriptRunner
  uia_session.{h,cpp}   # COM wrapper: find / click / type / screenshot
  script_runner.{h,cpp} # JSON-driven step dispatcher
  scripts/
    smoke.json
    settings_tour.json
    chat_round_trip.json
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
| `press_key`            | `key`, optional `modifiers` (`"CTRL"`, `"ALT"`, `"SHIFT"`, plus/comma separated) | Keys: `ENTER`/`RETURN`, `ESC`/`ESCAPE`, `TAB`, `SPACE`, `BACKSPACE`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `COMMA`, `PERIOD`, `F1`..`F11`. |
| `select_tab`           | `parent_aid` + `index` (zero-based), or `name` for label-bearing tabs | wxNotebook on Windows wraps SysTabControl32 which doesn't propagate tab labels to UIA Name -- so `parent_aid` + `index` is the reliable shape. The `name` shape works for wxAuiNotebook and other strips that DO surface labels. |
| `get_text`             | `automation_id`, optional `walk_subtree` | Reads a control's value (Edit / Text / Document patterns). |
| `assert_text_contains` | `automation_id`, `substring`, optional `walk_subtree`, `timeout_ms` | Polls until the substring appears or the timeout elapses. |
| `assert_visible`       | `automation_id`, `timeout_ms` | Element exists and isn't off-screen. |
| `screenshot`           | optional `name` (default `step_NN.png`) | `PrintWindow(PW_RENDERFULLCONTENT)` -- captures WebView2 content. |
| `sleep`                | `ms` (default 100) | Use sparingly -- prefer `assert_text_contains` for real waits. |
| `quit`                 | `wait_ms` (default 3000) | WM_CLOSE first, terminate on timeout. |

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
| `wxFrame`, `wxPanel`, `wxButton`, `wxTextCtrl`, `wxStaticText` | ✓ full |
| `wxToggleButton` (mode switcher) | ✓ -- exposes `TogglePattern`; the driver maps `click` to `Toggle` |
| `wxNotebook` tabs (Activity panel, Settings dialog) | ✓ -- tabs are `TabItem` controls; `select_tab` uses `SelectionItemPattern` |
| `wxWebView` (Edge backend) | ✓ -- DOM exposed as a UIA `Document` sub-tree; chat content readable via `walk_subtree` |
| `wxTreeCtrl` (file tree) | ✓ -- standard `Tree` / `TreeItem` |
| `wxTextCtrl` with `wxTE_RICH2` (chat input) | ✓ but Windows reports its Name as `"RichEdit Control"` (native RichEdit overrides wxAccessibility). Address by control type `document` under the chat panel: `{ parent_aid: "locus.chat.panel", control_type: "document" }`. |
| `wxStyledTextCtrl` (Scintilla, tool approval JSON pane) | ✗ limited -- use `SendMessage`/`SCI_GETTEXT` direct to the HWND if needed |
| `wxAuiNotebook` (main frame panels) | ✓ exposes `TabItem`; click via `select_tab` |

## Three demo scripts

| Script | What it covers |
|---|---|
| [`smoke.json`](scripts/smoke.json) | Launch, main window appears, three named panels reachable, screenshot, clean quit. |
| [`settings_tour.json`](scripts/settings_tour.json) | Ctrl+, opens Settings, click through LLM / Index / Tool Approvals / MCP Servers, screenshot each, Esc closes the dialog, quit. |
| [`chat_round_trip.json`](scripts/chat_round_trip.json) | Type into chat input, press Enter, assert the user bubble appears in the WebView. Does not require an LLM -- the user-side render happens before any server round-trip. |

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

1. Write `scripts/my_feature.json` -- copy `smoke.json` as a starting point.
2. If the steps need a widget that's not yet named, add an entry to
   [`src/frontends/gui/ui_names.h`](../../src/frontends/gui/ui_names.h) and
   wire it in the relevant `*_panel.cpp`. Rebuild `locus_gui` (the test exe
   build will rebuild it for you).
3. Run the script: `--script scripts/my_feature.json`.
4. Pass-rate matters. UIA timing is forgiving up to ~5 s on a fast machine;
   if your script flakes, audit the explicit waits (`assert_text_contains`,
   `assert_visible`) before reaching for `sleep` -- explicit waits keep
   scripts fast on a quick machine and stable on a slow one.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Every script fails with `init: UIA COM init failed` | COM apartment conflict -- a parent process initialised STA differently. Run the exe standalone. |
| `find: timeout waiting for element (automation_id=...)` | Widget exists but no `SetName()` call -- check `ui_names.h` and the panel's create method. |
| Settings dialog opens but tab clicks miss | The notebook AddPage label changed and the script's `name` arg drifted -- update the script to match the new label. |
| Screenshots show black rectangles for the chat WebView | Old PrintWindow without `PW_RENDERFULLCONTENT`. We pass that flag; if it ever fails, fall back to `BitBlt` after `RedrawWindow(RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW)`. |
| Process leaks between scripts | The harness sends `WM_CLOSE` then `TerminateProcess` on timeout. Each script gets its own workspace dir under `%TEMP%/locus_ui_tests/` so they don't compete for the WorkspaceLock. |
| `chat_round_trip` fails to find the user message | The WebView contents render async; `assert_text_contains` polls with a 10 s timeout. If your machine is loaded, raise the timeout to 30 s for that step. |

## Relationship to the other test suites

- **Unit tests (`locus_tests`)** -- fast, hermetic, no GUI. Untouched by S5.L.
- **Integration tests (`locus_integration_tests`)** -- live LLM, drives
  `AgentCore` end-to-end, no GUI. Untouched by S5.L.
- **UI automation tests (this)** -- drives the real wx widgets; LLM is
  optional (only the chat round-trip would benefit, and even there only for
  agent-side assertions which we deliberately don't make).
- **Manual tests (`tests/manual/`)** -- click-by-click QA plans that cover
  what UIA structurally can't (drag-and-drop visuals, focus order, file
  dialogs, third-party software workflows). UIA now covers the smoke +
  settings-tour baselines; new manual plans can focus on what UIA can't
  reach.
