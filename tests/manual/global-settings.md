# Global Settings + ~/.locus (S5.M)

Three tests covering the global settings template, the legacy `%APPDATA%\Roaming\Locus` -> `~/.locus/` migration, and the Open-Global-Config menu item.

`~/.locus/` resolves to `C:\Users\<user>\.locus\` on Windows.

## Test 1: First-open template flow

1. Open any existing workspace and click **File > Settings...**.
2. Change a few values across multiple tabs (e.g. LLM tab: bump Max tokens to `16384`; Capabilities tab: tick **Background processes**; Index tab: add `secret_dir/**` to Exclude patterns).
3. Click **Save as global defaults**. Confirm "Yes" on the dialog. Expect a success popup naming `C:\Users\<user>\.locus\config.json`.
4. Click **Cancel** (do NOT click OK -- this test verifies the global save is independent of the workspace save).
5. Confirm `C:\Users\<user>\.locus\config.json` exists and contains `"max_tokens": 16384`, `"background_processes": true`, and `"secret_dir/**"` in `exclude_patterns`. The file should NOT contain any `mcp:*` keys under `tool_approvals`.
6. Quit Locus. In Explorer, create a fresh empty folder (e.g. `C:\Temp\fresh-ws\`).
7. Launch Locus and open the fresh folder. Click **File > Settings...** -- the LLM tab shows Max tokens `16384`, Capabilities tab has Background processes ticked, Index tab lists `secret_dir/**`. The fresh workspace's `.locus/config.json` now contains the same values verbatim.

**Expected outcome**: the global file acts as a one-time template. Future workspaces inherit; existing workspaces are untouched.

## Test 2: Legacy folder migration

1. Quit Locus.
2. In Explorer, navigate to `C:\Users\<user>\AppData\Roaming\Locus\` (create the folder if it doesn't exist). Place a recognisable file at `mcp.json` (paste `{"mcpServers":{}}` into it) and another at `recent_workspaces.json` (paste `["C:\\Temp\\imaginary"]`). Optionally create a `prompts\hello.md` file.
3. **Delete** `C:\Users\<user>\.locus\` entirely if it exists (so the migration has work to do). NOTE: this wipes the global config from Test 1 -- only do this after Test 1 is complete.
4. Launch Locus.
5. Open a workspace. While running, confirm `C:\Users\<user>\.locus\` now exists and contains `mcp.json`, `recent_workspaces.json`, and (if you staged it) `prompts\hello.md`. The legacy `%APPDATA%\Roaming\Locus\` is left intact for inspection.
6. Quit and relaunch -- on the second launch the migration does not re-fire (look at `.locus/locus.log` for the workspace: there should be no "Migrated ... -> ..." line on the second startup).

**Expected outcome**: legacy files copied once, idempotent on subsequent launches. The user can delete the legacy folder by hand once they've verified the copy.

## Test 3: Open Global Config menu

1. With any workspace open, click **File > Open Global Config...**.
2. If `C:\Users\<user>\.locus\config.json` doesn't exist yet, Locus creates it with compiled defaults first, then opens it.
3. The default associated editor for `.json` opens with the file loaded (VS Code, Notepad++, Notepad -- whatever the user has registered).
4. Edit `"llm_endpoint"` to `"http://manual-test:9999"` and save the file.
5. Quit Locus. Create a fresh empty folder and open it as a new workspace.
6. **Settings > LLM** shows `http://manual-test:9999` in the Endpoint URL field, and `<fresh-ws>/.locus/config.json` contains the same value.

**Expected outcome**: hand-edits to the global file are picked up by the next new-workspace open. The change does not retroactively affect any existing workspace.
