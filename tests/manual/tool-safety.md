## Manual Test Plan -- Tool & Filesystem Safety (S5.O)

Three independent hardening fixes:

- **Path containment.** `resolve_path` (used by every file tool, the checkpoint
  snapshot path, and the auto-commit path) now walks path components instead of
  doing a raw byte-prefix compare. Sibling directories like `<workspace>-evil`
  are rejected even though they share a prefix with the workspace root.
- **Approval keying.** `ToolDispatcher` stores user decisions keyed by
  `call_id`. A late or duplicate click for one call can no longer wake a
  dispatch for a different call. Stale decisions are dropped with a
  warn-log; they don't sit in memory waiting to fire on the next prompt.
- **Atomic write_file.** `write_file` now writes through the same
  `<target>.locus-tmp` + rename helper `edit_file` already used. A process kill
  or power loss between truncate and final flush can't leave a half-written or
  zero-byte file on disk.

---

### Test 1 -- Outside-workspace path rejection

1. Open the Locus repo as a workspace (`D:\Projects\AICodeAss`).
2. Make sure a sibling folder exists with a prefix-overlapping name, e.g.
   `D:\Projects\AICodeAss-evil`. Put one file inside it: `sibling.txt`
   containing the word `bait`.
3. In chat, ask: `read_file ../AICodeAss-evil/sibling.txt`.

**Expected**

- The tool call appears in the chat / activity log; the result is an
  `Error: path resolves outside workspace` (or equivalent). The word
  `bait` does not appear in the response.
- Hard fail: if the model gets back `bait`, the sibling-prefix bug has
  regressed.

4. Ask: `read_file ..\..\Windows\System32\drivers\etc\hosts` (or any
   parent-traversal path).

**Expected**

- Same rejection. No host file contents in chat.

5. Ask: `read_file CLAUDE.md` (a real workspace-relative path).

**Expected**

- Tool returns the file's content normally -- the rejection only fires for
  paths that resolve outside the workspace.

---

### Test 2 -- Approval keyed by call_id

This exercises the dispatcher's mapping from decision to call. The bug is
latent today (tools run serially), so the test is essentially a smoke
that double-clicks no longer cause weirdness.

1. Open Locus on any workspace. Confirm `edit_file` is set to "Ask Every Time"
   in Settings -> Tool Approvals.
2. Ask the agent to make a small edit, e.g. `edit README.md to capitalise
   "the" -> "The" on line 1`.
3. When the approval panel opens, click **Approve** twice quickly (or
   Approve, then click Approve again on a now-closed panel via a stale
   click). Use the *visible* Approve button on the live panel.

**Expected**

- The edit lands once. No double application, no panic. Closing the
  approval panel and clicking Approve in the void produces no second
  edit and no error popup.
- If you have `.locus\locus.log` open with `-verbose`, the second
  (stale) click logs:
  ```
  [warning] ToolDispatcher: dropping stale tool decision for call_id '...' (no dispatch currently awaiting)
  ```

4. Open a second prompt right after: `edit README.md to capitalise the
   first letter of every line`. Approve normally. Confirm the second
   edit applies correctly -- the stale decision from Step 3 didn't
   shadow this turn's approval.

**Expected**

- Both turns commit their intended edits in order. The activity log
  reads tool_call ... tool_result ... tool_call ... tool_result with
  matching call_ids.

---

### Test 3 -- write_file atomicity under crash

This is a forced-crash test. It needs Task Manager (or
`Stop-Process -Force`). Pick a file you can replace from version control
if anything goes wrong, but the design says it will not.

1. Open Locus on the workspace. Disable auto-approve for `write_file`
   so the approval panel pops.
2. Ask: `write_file scratch/big.txt with a 50 MB block of "A"
   characters` (or any prompt that produces a big write -- the panel
   needs to render before the tool executes). If you'd rather not have
   the LLM compose 50 MB of "A", use a smaller payload; the test is
   about the rename happening atomically, not the size.
3. **Before** clicking Approve, open File Explorer to the
   `scratch/` folder. Note that no `big.txt` and no `big.txt.locus-tmp`
   exist yet.
4. Click Approve. Watch the folder:
   - For a brief moment a `big.txt.locus-tmp` may flash into existence
     and disappear; or it may complete so fast you only see the final
     `big.txt`.
5. Open `big.txt` -- it contains the requested content. No `.locus-tmp`
   sidecar remains.

**Expected**

- Target file present with the new content, no `.locus-tmp` sidecar.
- Hard fail: if `big.txt.locus-tmp` is still on disk after the call
  succeeded, the helper's rename / cleanup path regressed.

For the actual crash simulation:

6. Ask again: `write_file scratch/big.txt overwriting it with
   "REPLACED"`. Approve. This succeeds normally.
7. Verify `scratch/big.txt` contains `REPLACED`.
8. Ask once more for a big write to the same file. Click Approve.
9. **Immediately** kill `locus_gui.exe` via Task Manager (End task) or
   `Stop-Process -Force -Name locus_gui` from PowerShell.
10. Re-open Locus. Look at `scratch/big.txt`.

**Expected**

- The file is either the previous content (`REPLACED`) *or* the new
  full content -- never zero bytes, never partial. A stale
  `big.txt.locus-tmp` may linger; it's harmless and the next run is
  free to delete it manually.
- Hard fail: if the file is empty or contains only the first N MB of
  the new content with no trailing bytes, the atomic-write helper
  regressed (probably the rename step didn't fire and an old codepath
  is truncating the target directly).
