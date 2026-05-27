## Manual Test Plan -- Tool Approval Modal

**Feature:** S5.Z #3. The tool-approval surface is a modal dialog
(`ToolApprovalDialog`) that appears centred over the frame only when a
tool call needs a decision, then disappears entirely on response. There
is no permanent dock pane.

**Prerequisites:** at least one tool with approval policy `ask` (the
default for `edit_file` / `write_file` / `delete_file` / `run_command` /
`run_command_bg` unless you've changed Settings -> Tool Approvals).

---

### Test 1 -- Approve / Reject / X-close round trip

1. Open the Locus repo as a workspace.
2. Ask the agent: `please write hello world to a file called test.txt`.
3. When the LLM proposes the `write_file` call, the approval modal
   appears centred over the frame.

**Expected**

- Title bar reads `Tool Approval - write_file` (the tool name carries
  through so multiple pending calls can be told apart in Alt+Tab).
- Bold tool name + JSON args (Scintilla, syntax-coloured) + three
  buttons (`Approve (Enter)`, `Modify (M)`, `Reject (Esc)`) are visible.
- The chat panel BEHIND the modal already shows a `msg-tool` bubble
  for `write_file` (chat rendering happens unconditionally before the
  modal is shown).
- No bottom dock pane is open in the AUI layout. Resize the window:
  the chat pane uses the full height between sidebar and activity
  panel -- there is no empty space at the bottom that used to belong
  to the approval pane.
4. Click **Reject**. The modal disappears. The chat shows a tool
   result containing "rejected" or similar; the agent moves on or
   errors out (depending on what it tries next).
5. Re-prompt for the same `write_file`. When the modal reappears,
   click **Approve**. The modal disappears, the file is written, and
   the chat shows the success result.
6. Re-prompt one more time. When the modal reappears, click the **X**
   in the title bar (or press Alt+F4).

**Expected for step 6**

- The modal closes. The agent treats it as Reject (you'll see the
  rejection result in chat). Hard fail: if the chat hangs with no
  result and no further turns happen, the close-window handler
  failed to fire `tool_decision` and the dispatcher condvar is
  stuck (session is wedged; only fix is to quit and relaunch).

---

### Test 2 -- Modify path with edit_file diff view

1. Ask the agent: `please add a comment "// hello" at the top of CLAUDE.md`
   (or any other small `edit_file` request).
2. When the modal opens for `edit_file`, the args pane should show a
   coloured unified diff (red `-` lines, green `+` lines, header line
   per edit) instead of raw JSON. This is the existing S4.A diff
   renderer, unchanged by S5.Z #3.
3. Press **M** (or click Modify). The pane flips to editable JSON.
   The Approve button label changes to `Confirm (Enter)`.
4. Edit the JSON -- e.g. change the comment string to `// hi`.
5. Press **Enter** (or click Confirm).

**Expected**

- The modal disappears. The file is edited with the modified content.
- Hard fail: if the modal stays open after Confirm, the JSON parse
  failed silently. A wxMessageBox should appear if the JSON is
  invalid, NOT a stuck modal.

---

### Test 3 -- Multiple sequential approvals in one turn

1. Ask the agent something that requires two or more separate
   approvals, e.g. `please run "dir" then run "echo hello"` with
   `run_command` set to Ask. (Read tools are dispatcher-forced to
   auto_approve and never reach the approval modal regardless of the
   override map.)
2. After approving the first call, the modal should disappear and
   reappear for the second call.

**Expected**

- Each modal is independent (separate ShowModal call); the title
  bar and JSON args reflect the current call.
- After the final approval, the modal closes and stays closed
  through `turn_complete`. No flicker, no leftover empty dock slot.
- Hard fail: if the second modal's contents look stale (showing the
  first call's args), the dialog state isn't being reset between
  calls. Hard fail: if the AUI layout shifts or shows a brief empty
  pane between calls, the dock pane regression has come back.
