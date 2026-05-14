# Terminal Panel (S5.B) -- Manual Test Plan

Live, dockable terminal pane that streams stdout / stderr from every shell
command Locus runs (synchronous `run_command` and background `run_command_bg`),
with per-process tabs and ANSI colours. The panel is **hidden by default**;
View > Terminal toggles it.

## Test 1 -- Toggle, sync run_command, ANSI colour, scrollback

1. Open WS1. View > Terminal (Ctrl+`).
2. Expect: the **Terminal** pane docks at the bottom. One tab labelled
   **Run** is present and empty.
3. In the chat input, ask the agent to run `cmake --build build/release
   --config Release --target locus_tests` (approve the tool call).
4. Expect, while the build runs:
   - Output streams live into the **Run** tab -- compiler warning lines
     appear within ~1 s of being printed, not in one wall after exit.
   - The tab label updates to `Run *` (running badge) and switches to
     `Run [0]` (or `Run [N]` on failure) when the command exits.
   - ANSI colours from MSBuild / cl.exe (if any) render as coloured runs
     against the white background; plain stretches stay black.
   - `cmake --build` progress lines that use `\r` to overwrite (e.g.
     `[ 42%] Building CXX object ...`) replace the current line rather
     than stacking forever.
5. Scroll up mid-stream. Expect: new chunks stop auto-scrolling to the
   bottom. Scroll back down to the last line; auto-scroll resumes.
6. Right-click on the STC area (any) -- the context menu is currently
   provided by wxStyledTextCtrl's defaults (Copy / Select All / etc.).
   Hard fail: the panel must not crash on right-click.
7. Run the same command twice. Expect: the second run **clears** the Run
   tab before streaming, rather than appending. Output retained between
   runs is the most-recent invocation only.
8. View > Terminal again (Ctrl+`). Expect: the pane hides; toggling back
   on shows it with the last invocation's output still in the Run tab.
   Closing the pane with its X also unchecks the View menu item.

## Test 2 -- Background processes, per-tab lifecycle, scrollback cap

1. Open WS1. View > Terminal.
2. Ask the agent to start a long-running background command, e.g.
   `run_command_bg` with `command="cmd /c for /L %i in (1,1,5000) do
   @(echo line %i & ping -n 1 127.0.0.1 > nul)"`. Approve.
3. Expect:
   - A new tab `#1 cmd /c for /L %i in (1,1,...)` appears next to **Run**.
   - The badge is `*` (running) while the loop ticks.
   - Output lines stream into the tab at ~30 fps; the UI doesn't freeze.
4. Start a second background command (any short one, e.g.
   `run_command_bg "cmd /c dir"`). Approve. Expect: tab `#2 cmd /c dir`
   appears alongside `#1`. Switching between tabs preserves their
   independent buffers.
5. Let tab #2 finish naturally. Badge flips to `#2 ... [0]`.
6. Run `read_process_output` against #1 (LLM side). Expect: the LLM still
   reads from the same ring buffer; the panel's view is independent and
   shows everything, not just the un-delivered tail.
7. Ask the agent to `stop_process` on #1. Approve. Badge flips to
   `#1 ... [x:1]` (killed). The tab stays around so output remains
   inspectable.
8. Run a command that produces well over 10000 lines (e.g. a tight loop
   to 20000). Expect: the tab caps at ~10000 retained lines (newest at
   the bottom). The agent's `read_process_output` may still expose the
   exact LLM-facing ring buffer cap independently.

## Test 3 -- Close + cleanup

1. With the Terminal pane visible and one bg process still running, close
   the main window.
2. Expect: the process is terminated as part of `Workspace::~` (existing
   S4.I behaviour); the GUI exits cleanly without spdlog reports of
   late-fire callbacks into a destroyed panel.
3. Re-open Locus on the same workspace. Expect: the Terminal pane is
   hidden by default again (no AUI perspective persistence in v1) and
   does NOT remember the previous session's tabs.

---

## Known pitfalls (lessons from bugs already fixed)

These are documented because they cost real debugging time and the
symptoms are weirder than the underlying cause:

- **Lone `\r` vs CRLF**. The AnsiParser deliberately treats a bare `\r`
  as "erase the current line" so `cmake` / `ninja` progress bars
  (`[ 10%] Build` -> `[ 20%] Build`) overwrite correctly. Windows EOL
  is `\r\n`, though, so we must collapse the pair into a plain `\n`;
  otherwise every line of Python / Node / cmd output gets wiped by the
  `\r` between `text` and `\n`. The parser's `pending_cr_` flag holds
  the `\r` until it sees the next byte, including across chunk
  boundaries. Regression cases live in [tests/test_ansi_parser.cpp]
  ("CRLF collapses to one newline", "CRLF straddling a chunk boundary
  still collapses", "lone CR straddling a chunk boundary still erases").
  Symptom when broken: terminal shows a single gray rectangle (the
  trailing `\n` rendered as a control-char placeholder) on each line
  instead of the actual text.

- **Scintilla style-id range overlap (the `#B2182C` red-tab-bg bug)**.
  Scintilla reserves style ids 32-39 for `wxSTC_STYLE_DEFAULT`,
  `_LINENUMBER`, `_BRACELIGHT`, `_BRACEBAD`, `_CONTROLCHAR`,
  `_INDENTGUIDE`, `_CALLTIP`, `_LASTPREDEFINED`. `wxSTC_STYLE_DEFAULT`
  (id=32) is in particular the style Scintilla paints over the empty
  area below the last styled byte. The terminal panel encodes an
  `(fg, bg, bold)` AnsiStyle into a style id as
  `1 + fg + bg*17 + bold*289`. That formula collides with the reserved
  range -- e.g. `(fg=14, bg=1, bold=0)` produces id=32, and the
  ensure_style_table loop happily overwrites Scintilla's STYLE_DEFAULT
  with bg=palette(1)=`#B2182C` (ANSI red). The whole terminal pane
  then paints red.
  Fix: `style_id_for` bumps any id in 32-39 by +8 so ANSI-styled text
  lands at 40-47 and the reserved range stays untouched. The
  `ensure_style_table` loop tracks per-id assignment in a
  `vector<bool>` so a later high-id combo (the >254 clamp) can't
  overwrite an earlier valid style either.
  Symptom when broken: solid red (or whatever colour happens to be
  palette[N] where N is whatever bg-index the formula maps to id=32)
  in the entire terminal tab content area, regardless of any
  `SetBackgroundColour` calls anywhere in the widget tree. This
  almost nerd-sniped us into chasing the `wxWidgets#25678`
  dark-mode-notebook-bg issue, which sounds related but isn't --
  watch out for it.

- **`wxString::FromUTF8` returns empty on any decode failure**. If a
  process emits non-UTF-8 bytes (CP1252 / CP866 / OEM on Windows),
  passing the chunk through `FromUTF8` drops the entire chunk
  silently. `write_styled` now falls back to `From8BitData` (Latin-1)
  on empty-result, so ASCII output at least survives mixed-encoding
  streams.

## Out of scope for v1 (don't fail the plan over these)

- Sending input back to the running process (interactive prompts, `git
  push` password prompts). Documented limitation in the stage spec.
- AUI perspective persistence (the panel always starts hidden).
- Status-bar badge with active-process count.
- Search-in-tab via Ctrl+F (wxSTC's built-in incremental search can be
  used; no dedicated find bar).
- Save-to-file from the tab context menu.
