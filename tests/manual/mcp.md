# Manual Test Plan -- MCP Servers (Model Context Protocol)

**Feature:** Plug third-party MCP servers into Locus so their tools become
available to the AI agent alongside Locus's built-ins.

**Audience:** QA. No source-code reading required. All steps are GUI clicks or
text-file edits.

**Estimated time:** ~20 minutes for the full plan; ~5 minutes if you only run
the smoke test (Test 1 + Test 2).

---

## Prerequisites

Standard Locus dev setup (GUI built, LM Studio running with a tool-calling
model). One extra item for one test:

- **Test 6 only:** Node.js / `npx` available on PATH. Skip Test 6 if you
  don't have Node -- the other tests don't need it.

---

## Test 1 -- Empty state (no servers configured)

**Goal:** verify the MCP UI is reachable and gracefully reports "nothing
configured" when there's no `mcp.json`.

### Setup

1. In the workspace's `.locus/` folder, check whether `mcp.json` exists.
   If it does, rename it to `mcp.json.bak` for this test only.

### Steps

1. Launch Locus: `locus_gui.exe <workspace_path>` (or open it from the Start
   menu and pick the workspace).
2. Wait for the chat panel to render and the index to finish (the activity
   panel on the right will go idle).
3. From the menu bar choose **Edit -> Settings...** (or whatever keyboard
   shortcut the build advertises).
4. The Settings dialog opens. Confirm there are **four tabs at the top**:
   `LLM`, `Index`, `Tool Approvals`, `MCP Servers`.
5. Click the **MCP Servers** tab.

### Expected

- The tab shows the message
  *"MCP support is initialised by LocusSession at startup. Create
  .locus/mcp.json (or %APPDATA%/Locus/mcp.json) and restart Locus..."*
- No server list, no Restart button, no Trust checkbox.

### Cleanup

- Close the Settings dialog with **Cancel**.
- Leave Locus open for the next test.

---

## Test 2 -- Add a mock server (built-in test stub)

**Goal:** verify a configured server is detected, spawned, and its tools
register into Locus's tool catalog. Uses the bundled test mock so no third-
party software is required.

### Setup

1. Close Locus.
2. Locate the mock server: `build/release/tests/Release/locus_mock_mcp_server.exe`.
   Note the **absolute path** and write it with **forward slashes**, e.g.
   `D:/Projects/AICodeAss/build/release/tests/Release/locus_mock_mcp_server.exe`.

   > **JSON gotcha:** backslashes are escape characters in JSON, so writing the
   > path as `D:\Projects\...` will either fail to parse or silently produce
   > a wrong path (`\P`, `\A`, `\b`, `\r`, `\t` are all interpreted). Either
   > use forward slashes (recommended) or double every backslash
   > (`D:\\Projects\\...`).

3. In the workspace's `.locus/` folder, create a new file `mcp.json` with
   this content (replace `<PATH_TO_MOCK>` with the forward-slash path you noted):

   ```json
   {
     "mcpServers": {
       "mock": {
         "command": "<PATH_TO_MOCK>"
       }
     }
   }
   ```

### Steps

1. Launch Locus again with the same workspace.
2. After startup, open **Edit -> Settings... -> MCP Servers**.
3. Look at the server list.

### Expected

- One row: `mock | ready | 2 | <path to locus_mock_mcp_server.exe>`.
- Selecting the row populates the detail box at the bottom with something
  like:

  ```
  Status: ready
  Tools: mcp:mock:echo, mcp:mock:fail_once
  ```

- Buttons **Restart** and **Open mcp.json** are enabled.
- The **Trust selected server** checkbox is unticked.

### Cleanup

- Close the Settings dialog with **Cancel**.
- Leave Locus open.

---

## Test 3 -- Use an MCP tool via the AI agent

**Goal:** end-to-end smoke. The LLM calls an MCP tool, the result lands in
chat, and the approval gate handles it correctly.

### Setup

- Test 2's `mcp.json` is in place; Locus is open; LM Studio is running.

### Steps

1. In the chat input, paste exactly:

   > Use the `mcp:mock:echo` tool with argument `text="hello manual test"`.
   > Do not paraphrase or summarise. Call exactly that tool, exactly once.

2. Press Enter.
3. **Approval gate:** the tool-approval pane (bottom of the window) should
   pop up showing `mcp:mock:echo` and the arguments JSON
   `{"text":"hello manual test"}`. Click **Approve**.
4. Wait for the agent to finish (the chat footer will say `Idle` or similar).

### Expected

- The chat shows a tool call header `mcp:mock:echo` followed by a tool
  result containing the text `hello manual test`.
- The assistant's reply afterwards mentions the echoed text.
- The activity panel on the right shows a `tool_call -> tool_result`
  sequence for `mcp:mock:echo`.
- If the model used the wrong tool (e.g. `read_file`) or no tool at all,
  this is a small-model variance -- retry once. If it consistently fails,
  log it as a model-side issue rather than an MCP bug.

### Cleanup

- None needed.

---

## Test 4 -- Trust toggle (auto-approve a server)

**Goal:** verify that the per-server Trust checkbox skips the approval gate
for every tool from that server.

### Setup

- Test 2's `mcp.json` is in place; Locus is open.

### Steps

1. Open **Edit -> Settings... -> MCP Servers**.
2. Select the `mock` row.
3. Tick **Trust selected server (auto-approve all mcp:&lt;server&gt;:* tools)**.
4. Click **OK** to save.
5. In the chat input, paste:

   > Use `mcp:mock:echo` with `text="trusted call"`.

6. Press Enter.

### Expected

- **No approval pane appears.** The tool call runs immediately.
- The chat shows the call + result with `trusted call` echoed back.

### Verify the setting was persisted

7. Close Locus completely.
8. Open the workspace's `.locus/config.json` in a text editor.
9. Find the `tool_approvals` section.

### Expected (continued)

- The map contains an entry `"mcp:mock:*": "auto"`.

### Cleanup

- Reopen Locus.
- Settings -> MCP Servers -> select `mock` -> **untick** Trust -> **OK**.
- Confirm `config.json` no longer has the `mcp:mock:*` entry.

---

## Test 5 -- Restart button

**Goal:** verify hot-restart picks up a changed server config without
restarting Locus.

### Setup

- Test 2's `mcp.json` is in place; Locus is open; the `mock` server is
  showing `ready`.

### Steps

1. Open **Edit -> Settings... -> MCP Servers**.
2. Select `mock`. Note the row says `ready, 2 tools`.
3. Click **Restart**.

### Expected

- After a brief pause (< 2 seconds), the row still shows `ready, 2 tools`.
- The detail box still lists `mcp:mock:echo` and `mcp:mock:fail_once`.

### Negative variant (optional)

4. Without closing Settings, open `.locus/mcp.json` in another editor and
   temporarily corrupt the command path (e.g. change `locus_mock_mcp_server.exe`
   to `nope.exe`).
5. Back in Settings, click **Restart** again.

### Expected

- The status flips to `failed`.
- The detail box shows a `Last error:` line mentioning `CreateProcess failed (2)`.
- A popup confirms the failure.

### Cleanup

- Fix `mcp.json` back to the original path.
- Click **Restart** -- the row returns to `ready, 2 tools`.
- **OK** out of Settings.

---

## Test 6 -- A real third-party MCP server (npx filesystem)

**Goal:** prove Locus speaks MCP correctly against a real shipping server,
not just our test mock. Requires Node.js / npx.

### Setup

1. Close Locus.
2. Replace `.locus/mcp.json` with:

   ```json
   {
     "mcpServers": {
       "fs": {
         "command": "npx",
         "args": ["-y", "@modelcontextprotocol/server-filesystem", "."],
         "init_timeout_ms": 60000
       }
     }
   }
   ```

   The high `init_timeout_ms` covers the first-run npm download.

### Steps

1. Launch Locus. The first launch will take ~30 seconds while npx pulls
   the package; subsequent launches are fast.
2. Open **Edit -> Settings... -> MCP Servers**.

### Expected

- One row: `fs | ready | <several tools> | npx`.
- Selecting it lists a handful of `mcp:fs:*` tools (read_file, list_directory,
  search_files, etc.).

### End-to-end use

3. Close Settings.
4. In the chat input:

   > Use `mcp:fs:read_file` to read the file `README.md` in this workspace.

5. Approve the tool when the gate pops up.

### Expected (continued)

- The result shows the contents of `README.md`.
- The agent summarises or quotes from the README.

### Cleanup

- Settings -> MCP Servers -> select `fs` -> uncheck Trust if set.
- Close Locus.
- Restore `.locus/mcp.json` to the previous version (or delete it for the
  next tester).

---

## Test 7 -- Server fails to start (error path)

**Goal:** verify a misconfigured server is shown clearly without crashing
Locus or breaking the rest of the agent.

### Setup

1. Close Locus.
2. Set `.locus/mcp.json` to:

   ```json
   {
     "mcpServers": {
       "nope": { "command": "this-binary-does-not-exist.exe" }
     }
   }
   ```

### Steps

1. Launch Locus.
2. Wait for startup to complete -- the chat should still load normally.
3. Open **Edit -> Settings... -> MCP Servers**.

### Expected

- The row shows `nope | failed | 0 | this-binary-does-not-exist.exe`.
- The detail box's `Last error:` line mentions
  `CreateProcess failed (2)` or similar.
- **Restart** is still available; clicking it produces the same failure
  and a popup.
- The chat still works against the built-in tools (open the chat and ask
  the agent to list this workspace's top-level files; it should succeed).

### Cleanup

- Close Locus.
- Restore `.locus/mcp.json` (or delete it).

---

## Test 8 -- "Open mcp.json" creates a stub

**Goal:** verify the convenience button gets the user started on a fresh
workspace.

### Setup

1. Close Locus.
2. In `.locus/`, rename any existing `mcp.json` aside (e.g. to `mcp.json.bak`).

### Steps

1. Launch Locus.
2. **Edit -> Settings... -> MCP Servers**.
3. Click **Open mcp.json**.

### Expected

- A new `.locus/mcp.json` is created with a placeholder stub.
- The file opens in your system default editor for `.json` files.
- The stub contains `{ "mcpServers": { ... } }` with a commented example
  entry.
- After closing the editor and clicking **Restart** (still no servers
  configured), the list stays empty (no errors).

### Cleanup

- Delete the stub `mcp.json` (or replace it with your previous content).

---

## When you're done

- Restore `.locus/mcp.json` to whatever the developer asked for (usually:
  absent, or whatever was checked in).
- Make sure `.locus/config.json`'s `tool_approvals` section doesn't carry
  stray `mcp:*:*` entries left over from your testing.

If any test failed, capture:

- The tail of `.locus/locus.log` (last ~200 lines).
- The exact `mcp.json` you had configured.
- A screenshot of the Settings -> MCP Servers tab at the time of failure.
