# Workspace Capabilities (S5.A) -- Manual Test Plan

Five capability buckets gate families of tools so the per-turn manifest
only carries what the workspace actually uses. The choice surfaces twice:
on the very first open of a workspace (modal) and in Settings ->
Capabilities (tab).

## Test 1 -- First-open modal, end-to-end manifest effect

1. Pick a fresh folder that has never been opened by Locus. Confirm
   there is no `.locus/` directory inside it. Launch the GUI on this folder.
2. Expect: the **Workspace Capabilities** modal appears before the
   main window. Five rows:
   - Background processes (~700 prompt tokens) -- unchecked
   - Semantic search (~80 prompt tokens) -- checked
   - Code-aware search (~250 prompt tokens) -- checked
   - Memory bank (~300 prompt tokens) -- checked
   - Web retrieval (~300 prompt tokens) -- unchecked
3. Uncheck Code-aware search + Memory bank. Click OK. Main window opens.
4. Open `.locus/config.json` in an external editor (or via View > File
   tree). Expect the new `capabilities` block reflects the choices, plus
   the legacy `index.semantic_search.enabled` and `memory.enabled` mirrors
   are populated.
5. Ask the agent any question that triggers a tool. In the Activity panel,
   inspect the first "Tool manifest: ..." row of the next turn.
   Expect: the row count drops from the default ~17 tools to ~13 (no
   `get_file_outline`; no `add_memory` / `search_memory`); the `search`
   tool's description text no longer mentions `symbols` / `ast`. The
   `locus.log` line carries `(capabilities: -bg +sem -code -mem -web)`.
6. Hard fail: if the manifest still contains `get_file_outline` or the
   memory tools after the turn ticks, the capability gate didn't fire.

## Test 2 -- Settings tab round-trip + failure-mode message

1. With the workspace from Test 1 already open, hit Ctrl+, to open
   Settings. Switch to the **Capabilities** tab.
2. Expect: five checkboxes reflect the current state (Code-aware off,
   Memory off, rest at defaults from Test 1). Hover each label -- the
   tooltip names the gated tools.
3. Enable Background processes. Click OK.
4. Ask the agent to run a background command (e.g. "start a `cmd /c timeout
   5` in the background"). Approve the tool call. Expect: it succeeds --
   the bg-process tools are back in the manifest after the Settings save.
5. Re-open Settings -> Capabilities. Disable Background processes. Click OK.
6. Without restarting, ask the agent to call `run_command_bg` again (you
   may need to phrase the request to force it). Expect: the agent either
   doesn't see the tool (preferred -- the next turn's manifest is
   pruned), or, if its prior context still remembers the tool and it
   tries to call it anyway, a single Activity-log warning row appears:
   "Tool 'run_command_bg' is disabled in this workspace" with detail
   "Background processes is off. Enable it in Settings -> Capabilities."
   No generic "unknown tool" error.

## Test 3 -- Legacy workspace migration (no capabilities block)

1. Close Locus. Edit a workspace's `.locus/config.json` by hand: delete
   the `capabilities` object entirely so the file looks like a pre-S5.A
   config (keep the `index.semantic_search.enabled` and `memory.enabled`
   keys with whatever values you want).
2. Re-open the workspace in the GUI. Expect: NO first-open modal pops
   (the file existed, so S5.A treats it as an upgrade, not a new workspace).
3. Open Settings -> Capabilities. Expect the checkboxes are populated:
   - Semantic search reflects the legacy `index.semantic_search.enabled`
   - Memory bank reflects the legacy `memory.enabled`
   - The other three buckets carry the static defaults (bg off, code on,
     web off).
4. Click OK without changes. Open `config.json` again. Expect: the file
   now contains a fully-populated `capabilities` block alongside the
   legacy fields (both kept in sync on save).

## Out of scope for v1 (don't fail the plan over these)

- Live system-prompt swap mid-turn when memory_bank toggles. The
  in-context memory slot for the current session was captured at agent
  construction; a memory_bank toggle only affects the next session's
  prompt (intentional -- protects the KV cache, see S4.F).
- Web retrieval bucket has no tools to gate yet (M6 placeholder).
