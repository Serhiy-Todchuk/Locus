# Permission Presets (S5.S) -- Manual Test Plan

Four named per-tool approval presets + a Custom sentinel. The choice surfaces
in two places: Settings -> Tool Approvals (persistent) and the chat-footer
dropdown next to the context meter (runtime override; lasts until the tab is
closed).

## Test 1 -- Settings preset switch, round-trip via config.json

1. Open Locus on any workspace. Hit Ctrl+, and switch to the **Tool Approvals**
   tab. Expect a "Preset:" dropdown at the top of the tab with one of the
   labels selected -- typically "Ask before edits" for a fresh workspace.
2. Pick "Read-only" from the preset dropdown. Expect: no warning modal (the
   safe-direction picks don't prompt). The per-tool dropdowns below repaint
   so write_file / edit_file / delete_file / run_command / run_command_bg /
   add_memory / stop_process all show "Deny". `ask_user` stays
   "Ask Every Time". Read tools (read_file / search_* / list_directory /
   get_file_outline / list_processes / read_process_output / search_memory)
   are not in the per-tool grid -- they're always auto-approved and a
   one-line notice at the top of the tab calls that out.
3. Click OK. Re-open Settings -> Tool Approvals. Expect: the preset
   dropdown still reads "Read-only", and the per-tool dropdowns still
   reflect the snapshot.
4. Open `.locus/config.json` and confirm the new `tool_approvals` map
   includes explicit entries for every mutating tool (`"deny"`) plus an
   `"mcp:*": "ask"` wildcard. Read tools, plan tools, and ask_user don't
   appear -- reads are dispatcher-forced to auto regardless of any entry,
   and the other two were already at their built-in defaults.
5. Re-open Settings -> Tool Approvals. Pick "Allow all". Expect: a warning
   modal -- "Apply 'Allow all' preset?" with copy that calls out
   shell auto-approval and MCP still asking. Click "Apply preset".
   Per-tool dropdowns repaint -- run_command / run_command_bg / delete_file
   / write_file / edit_file all show "Auto-Approve". Click OK.
6. Open the **Tool Approvals** tab a third time. Switch the per-tool dropdown
   for `delete_file` from Auto-Approve to Ask Every Time. The preset
   dropdown at the top should now read **Custom** (with the description
   line "Per-tool overrides do not match any named preset.").

Hard fail: if the per-tool table is in the Allow-all shape but the
preset dropdown still reads "Allow all" after the user manually changed a
cell, detection is broken.

## Test 2 -- Chat-footer runtime override + settings-save reset

1. Settings should still be on "Allow all" from Test 1 step 5. Close the
   Settings dialog if open. Look at the chat footer next to the context
   meter -- expect a permission preset chip + dropdown. The chip text
   should read "Allow all" (since the settings dropped you there).
2. From the chat-footer dropdown, pick "Read-only". Expect a no-prompt
   change -- safe-direction. Chip text updates to "Read-only", colour is
   gray.
3. Open the Activity panel. Expect a single warning-kind row near the top:
   "Permission preset: Read-only (runtime override)" with the old + new
   preset names in the detail.
4. Ask the agent to do something destructive that would normally run
   (e.g. "delete `tests/scratch/temp.txt`"). The agent will emit a
   `delete_file` tool call. Expect the dispatcher refuses it with
   "Tool call denied by workspace approval policy" -- the runtime override
   is in effect even though `tool_approvals` on disk still says auto.
5. Re-open Settings -> Tool Approvals (Ctrl+,, tab 4). Without changing
   anything, click OK. Expect: chat-footer chip + dropdown snap back to
   "Allow all" (the saved baseline). Activity panel shows
   "Permission preset: runtime override cleared".
6. Pick "Allow all" again from the chat-footer dropdown. Expect: warning
   modal pops, with copy about shell auto-approval + MCP still asking.
   Click Cancel. Expect: chip stays at the previous effective value.

## Test 3 -- AFK-autonomous combo (Allow all + auto-compaction)

1. From Settings -> Tool Approvals, switch back to "Ask before edits" and
   click OK so the persistent baseline is the safe default again.
2. From the chat-footer dropdown, pick "Allow all". Confirm the warning
   modal. The chip is orange and reads "Allow all". Hover -- tooltip:
   "Files + shell auto-approved; MCP still asks (runtime override --
   closes with this tab)".
3. In the chat-footer auto-compact checkbox, ensure auto-compact is on.
4. Give the agent a multi-step task that requires several file edits + a
   shell command (e.g. "write a hello-world script at
   `tests/scratch/hello.sh`, then run it"). Expect: each tool call runs
   without an approval prompt; no modal interruption.
5. Then ask: "tell me your name using ask_user". Expect: even at "Allow
   all", `ask_user` still pauses for an answer -- the approval modal
   appears. This is the documented escape from full autonomy.
6. Close the tab via Ctrl+W. Open a new tab on the same workspace.
   Expect: the chat-footer chip on the new tab reads "Ask before edits"
   (the runtime override was per-tab; it doesn't carry over).

## Out of scope for v1 (don't fail the plan over these)

- Per-server MCP elevation via the chat footer. The chat-footer dropdown
  controls the wildcard category only; per-server `mcp:<name>:*` overrides
  stay under Settings -> MCP.
- Time-limited "Allow all for 30 minutes then revert" -- intentionally
  deferred; user re-picks "Ask before edits" when done.
- A loud "ELEVATED" red banner across the top of the workspace. Today the
  chip colour (orange / red-for-Custom-with-MCP-auto) is the only signal.
