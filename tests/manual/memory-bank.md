# Manual Test Plan -- Memory Bank

**Feature:** A workspace-scoped, persistent, searchable memory bank. The user
commits short verbatim notes the agent should remember; pinned + recently-used
entries auto-inject into the system prompt at session start, and an
`add_memory` / `search_memory` tool pair lets the agent grow + retrieve memory
on its own. Entries survive between sessions and are stored as plain
markdown files under `.locus/memory/`.

**Audience:** QA. Click-by-click; assumes basic familiarity with the chat
panel, activity panel, and reading `.locus/locus.log`.

**Estimated time:** ~15 minutes for the full plan; ~5 minutes for Tests 1+2.

**Phase note:** The Settings -> *Memory Bank* tab and the chat right-click
*Save as memory* are deferred to Phase 2 and are not yet present. Verify
everything below through the chat input, the activity panel, and the
on-disk `.locus/memory/` folder.

---

## What you're looking at

User-facing surfaces:

- **`/memorize [+tag1 +tag2] [--pin] <content>`** -- chat-input slash. Saves
  the rest of the line verbatim. Multiple `+tag` tokens accumulate; `--pin`
  sets the entry to "always loaded into the next session's prompt".
- **`/forget <id> [--hard]`** -- chat-input slash. Soft delete by default
  (moves the file under `.locus/memory/.deleted/` for 30-day retention);
  `--hard` removes permanently.
- **Agent-driven tools** (the LLM may pick these on its own):
  - `add_memory` -- approval policy *Ask Every Time*. Triggers the
    approval pane.
  - `search_memory` -- auto-approve. Runs without a popup; shows up
    in the activity panel as a `tool_call` / `tool_result` pair.
- **Activity-panel kinds** new in this stage: `memory_added`,
  `memory_searched`, `memory_deleted`. The first one fires from
  `/memorize`; the third fires from `/forget`.

On disk (under the workspace's `.locus/memory/`):

- `<id>.md` per live entry, where `<id>` looks like
  `2026-05-12-194841-5b44`. The file has a YAML frontmatter block
  (id / created_at / updated_at / last_used_at / source / pinned / tags)
  followed by the verbatim content.
- `.deleted/<id>.md` for soft-deleted entries.

---

## Test 1 -- /memorize round-trip and on-disk format

**Goal:** verify the simplest path: user types a slash, an entry lands on
disk, and the chat shows a confirmation.

### Setup

1. Open any workspace (the Locus repo `D:\Projects\AICodeAss\` is fine).
2. In Explorer, navigate to `.locus/memory/`. If it doesn't exist, that's
   fine -- Locus creates it on first save. If it has existing entries, just
   note the current file list so you can identify what this test adds.
3. Open `.locus/config.json` and confirm `memory.enabled` is `true` (default).
   If absent or `false`, set to `true` and restart Locus.

### Steps

1. In the chat input, type exactly:

   > `/memorize +qa +smoke --pin manual-test-marker is the canonical QA marker`

   and send.
2. Confirm the chat shows a confirmation line beginning with
   `Saved memory:` followed by an id like `2026-05-12-194841-5b44`. The
   line should also include `(pinned)` and `tags=qa,smoke`.
3. Type a second entry without flags:

   > `/memorize manual-test-plain has no tags`

   and send. Confirm the confirmation line has **no** `(pinned)` and **no**
   `tags=`.
4. Open the activity panel (right pane). Look for two `memory_added`
   events near the top, each with an `memory:<id>` summary.
5. Open Explorer at `.locus/memory/`. Confirm:
   - Two new `<id>.md` files have appeared.
   - Open the pinned one in a text editor. The frontmatter should be
     readable, e.g.

     ```
     ---
     id: 2026-05-12-...
     created_at: 2026-05-12T...
     ...
     source: user
     pinned: true
     tags: [qa, smoke]
     ---
     manual-test-marker is the canonical QA marker
     ```

     The body must be **byte-identical** to what you typed (no extra
     whitespace, no auto-newline, no escaping).
   - The unpinned one has `pinned: false` and `tags: []`.

### Expected

Two `memory_added` activity events, two `.md` files, exact verbatim content
in both. No `.locus/memory/.deleted/` activity yet.

### Cleanup

Leave the entries in place; Test 3 deletes them.

---

## Test 2 -- Persistence across sessions + auto-injected memory slot

**Goal:** confirm the "memory survives between sessions" promise and verify
the in-context slot picks up pinned entries on the next session start.

### Setup

Test 1 leaves two entries (`manual-test-marker` pinned, `manual-test-plain`
unpinned). Keep them.

### Steps

1. Close Locus completely (File -> Quit, or close the main window). Wait a
   couple of seconds for the workspace lock to release.
2. Re-open Locus on the same workspace.
3. After the workspace finishes opening, open the activity panel.
4. Find the `system_prompt` entry (it's the very first activity event). Click
   to expand. The full system prompt is shown as the `detail` body.
5. Scroll through the prompt and confirm a `## Memory Bank` section appears,
   followed by your **pinned** entry verbatim. The unpinned `manual-test-plain`
   entry may also appear (depending on `last_used_at` order and budget).
6. In the chat, paste this prompt verbatim:

   > Without calling any tool, tell me what the canonical QA marker is.
   > If you don't know, say "I don't know".

7. Send. The agent should answer with `manual-test-marker` quoted somewhere
   in its reply, **without** calling `search_memory` (that's the whole
   point of the in-context slot -- it's already in the prompt). If the agent
   calls `search_memory` and *then* answers, that still passes -- it means
   the auto-injection was either under budget or the model didn't trust it.

### Expected

`## Memory Bank` section present in the system prompt; agent recalls
`manual-test-marker` either from the slot or via search.

### Soft-pass notes

- Small models sometimes ignore the slot and call `search_memory` even
  though the answer is in the prompt. As long as the marker reaches the
  agent's reply, that's a pass.

### Hard fails

- `## Memory Bank` section is absent from the system prompt despite the
  pinned entry being present on disk.
- Agent confidently asserts "I don't know" without consulting memory at all.

---

## Test 3 -- /forget soft and hard

**Goal:** verify both deletion paths and the 30-day retention bin.

### Setup

The two entries from Test 1 should still be present. Copy both ids from the
`Saved memory:` lines in the chat history (or read them off the filenames
in `.locus/memory/`).

### Steps

1. In the chat input, type:

   > `/forget <id-of-manual-test-plain>`

   (substitute the actual id). Send.
2. Confirm the chat shows `Forgot memory:<id> (moved to .deleted/; restore
   with the Settings panel)`. The `memory_deleted` event appears in the
   activity panel.
3. In Explorer, confirm:
   - `.locus/memory/<id>.md` is gone.
   - `.locus/memory/.deleted/<id>.md` exists (the same file, just moved).
4. Now hard-delete the pinned entry:

   > `/forget <id-of-manual-test-marker> --hard`

5. Confirm the chat shows `Forgot memory:<id> (permanently)`.
6. In Explorer, confirm:
   - `.locus/memory/<id>.md` is gone.
   - `.locus/memory/.deleted/<id>.md` is **also** gone (hard delete bypasses
     the retention bin).
7. As a quick negative test, in the chat input type:

   > `/forget definitely-not-a-real-id`

   and send. Confirm the chat shows an **error** line (red / "Error" prefix
   depending on frontend) saying *"no memory entry with id ..."*.

### Expected

Soft delete leaves a copy under `.deleted/`; hard delete leaves nothing.
Unknown-id errors are reported via the error channel, not silently swallowed.

### Cleanup

The soft-deleted entry will age out after 30 days automatically. If you
want to clean up immediately, manually delete the `.locus/memory/.deleted/`
folder.

---

## Test 4 -- Agent picks `add_memory` (approval pane)

**Goal:** confirm the agent-driven path works and the approval gate fires
for the durable mutation.

### Setup

Standard workspace (Locus repo is fine). LM Studio running with a
tool-calling model loaded.

### Steps

1. In the chat input, paste verbatim:

   > Use the add_memory tool exactly once to save the following note
   > verbatim, then reply with the single word DONE.
   >
   > Note: "manual-test-agent-add reproduces add_memory through the tool
   > path."

2. Send. The agent should call `add_memory`.
3. The **approval pane** at the bottom should pop with a JSON args view:
   ```json
   {
     "content": "manual-test-agent-add reproduces add_memory through the tool path."
   }
   ```
   (Tags / pinned may or may not be present depending on the model.)
4. Click **Approve**.
5. The tool fires, the chat shows `Saved memory:<id>...`, and the agent
   eventually replies `DONE`.
6. Open `.locus/memory/<id>.md` and confirm:
   - `source: agent` (not `user` -- the agent saved this one).
   - The body contains the marker `manual-test-agent-add`.
7. Now ask the agent to retrieve it:

   > Use the search_memory tool to find anything mentioning the marker
   > `manual-test-agent-add`. Then in your reply quote the marker so I
   > know you found it.

8. The agent calls `search_memory`. The approval pane should **not** pop
   (auto-approve). The activity panel shows the `tool_call` + `tool_result`
   pair.
9. The reply quotes the marker. Pass.

### Expected

Approval pane pops only for `add_memory`. `source: agent` on the saved
file. `search_memory` retrieves the entry and the marker reaches the
model's reply.

### Hard fails

- Approval pane does not pop for `add_memory`. Check Settings -> Tool
  Approvals; `add_memory` should be set to *Ask Every Time*.
- `search_memory` triggers the approval pane (it should be auto-approve).

### Cleanup

In the chat, soft-delete or hard-delete the agent-added entry via
`/forget <id>` if you don't want it lingering.

---

## Test 5 -- Disabling memory removes the surface entirely

**Goal:** confirm the `memory.enabled = false` switch genuinely turns off
all three surfaces (tools, slash, prompt slot).

### Setup

1. Close Locus.
2. Edit `.locus/config.json` and change `memory.enabled` from `true` to
   `false`. Save.

### Steps

1. Re-open Locus on the same workspace.
2. Open the activity panel. Expand the `system_prompt` entry.
3. Confirm `## Memory Bank` is **absent** from the prompt body, regardless
   of any pinned entries on disk.
4. In the chat, type:

   > `/memorize this should not be saved`

5. Confirm an **error** line: *"Memory bank is disabled for this workspace."*
   No new file appears in `.locus/memory/`.
6. In the chat, ask:

   > List the tools you have available. Do you have anything for saving
   > or searching personal notes?

7. The agent should reply enumerating tools, with **no** `add_memory` and
   **no** `search_memory` in the list. If it mentions them, the manifest
   filter is broken -- capture the system prompt body and report.

### Expected

All three surfaces vanish: slot suppressed, `/memorize` rejected, both tools
filtered out of the manifest.

### Cleanup

1. Close Locus.
2. Edit `.locus/config.json` and set `memory.enabled` back to `true`.
3. Re-open Locus.

---

## Cross-test gotchas

- **Hot toggle is not supported.** Changing `memory.enabled` while Locus is
  running has no effect; you must close and reopen the workspace.
- **Embeddings.** When semantic search is enabled (default), every
  `add_memory` / `/memorize` triggers a synchronous embedder call on the
  agent thread. Expect ~50-200 ms latency per save on the default model;
  saves are instant only when semantic search is off.
- **File watcher ignores `.locus/memory/`.** The indexer's exclude patterns
  cover it, so editing memory `.md` files by hand does **not** trigger an
  index update. If you edit a memory entry directly on disk, close + reopen
  the workspace to make Locus re-read.
- **Pinned entries survive GC unconditionally.** If you've got 300+ entries
  and want some to "stay forever", pin them.
