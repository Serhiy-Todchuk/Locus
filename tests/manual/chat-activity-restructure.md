# Chat / Activity Restructure (S5.G)

Three pieces to verify end-to-end:

1. The collapsible **system-prompt bubble** at the top of the chat with the
   per-section token breakdown.
2. The hover-reveal **X delete** button on user and final-assistant chat
   bubbles + confirm dialog + history sync.
3. The **embedding-queue activity rows** that show up when chunks are
   queued + when batches drain.

Run against WS1 (`d:/Projects/AICodeAss`). Semantic search must be enabled
so the embedding worker actually runs; the first-open Capabilities modal
keeps it on by default.

## Test 1 -- system-prompt bubble + token chips

1. Launch the GUI: `build/release/Release/locus_gui.exe d:/Projects/AICodeAss`.
2. Wait for the workspace to finish indexing (Activity panel "Initial index
   complete" row). The chat shows a single greyed bubble at the top:
   **"> System prompt (N tokens)"** -- N is non-zero.
3. Click the bubble's header. It expands to:
   - A row of grey chips: at least **Base**, **Metadata**, **LOCUS.md**,
     **Tools**, plus optional **Memory** and **Format** chips. Their
     numbers must sum to N.
   - A scrollable code-style pane with the verbatim system prompt text.
4. Click the header again -- the bubble collapses back, chevron flips
   `v` -> `>`.
5. Type "hello" and submit. The user bubble shows a `(N t)` chip in its
   bottom corner. After the assistant replies, the assistant bubble also
   carries a `(N t)` chip.
6. If the model called a tool, the tool result bubble also carries a `(N t)`
   chip estimated from the displayed result text.

**Hard fails:** sum of chip numbers doesn't match the bubble header total;
the chip cluster is missing for a section that should be present (e.g.
**Tools** is 0 -- there are always built-in tools); the bubble re-appears
after `/reset` without re-rendering (it must re-render with the same hash).

## Test 2 -- per-message delete (hover + confirm + sync)

1. Continue from Test 1 with a non-trivial conversation (at least one user
   message and one final assistant reply). Make sure the assistant's reply
   did NOT call tools (deletable only fires on final-assistant turns).
2. Hover over the user bubble. A small circular `x` button fades in at the
   top-right corner of the bubble. Move the mouse away -- it fades back out.
3. Click the `x`. A modal dialog appears: "Delete this message? Subsequent
   turns will no longer see it." Click **No** -- the bubble stays.
4. Click the `x` again, this time click **Yes**. The bubble disappears
   immediately. The footer context meter drops by the deleted tokens.
5. The Activity panel shows a new `index_event` row: **"Deleted history
   message"** with detail `history_id=N`.
6. Hover over the final assistant bubble -- it also has an `x`. The
   bubble for an assistant message that called tools (an intermediate
   round) does NOT have an `x`. Neither does the system-prompt bubble
   nor any tool-result bubble.
7. Submit a follow-up message. The LLM no longer "remembers" the deleted
   turn -- e.g., if the deleted message was "my name is Bob" and you ask
   "what is my name?", the model says it doesn't know.

**Hard fails:** the `x` shows up on the system-prompt or tool bubble (must
not); clicking Yes doesn't remove the bubble (DOM out of sync); the
context-meter doesn't update after delete; the next agent turn still sees
the deleted message in its prompt.

## Test 3 -- embedding-queue activity rows

1. While the workspace is open, edit a few text files outside the GUI (touch
   a `.md` file in your terminal, for example).
2. Wait ~5 seconds for the file watcher to debounce and the indexer to
   process the batch.
3. The Activity panel shows new rows under the `index_event` kind:
   - **"Embedding queue: +N chunk(s) (done/total)"** -- fires on enqueue
     for each batch (the file you just touched produced N chunks).
   - **"Embedding progress: N/M"** -- fires periodically while the worker
     drains the queue. With the default throttle, expect one row per 100
     chunks or per 5 seconds, plus one final row when the queue hits
     `done == total` with `(done)` appended.
4. Click an `Embedding progress` row. The detail pane below shows the
   per-row delta + done + total counts.

**Hard fails:** no embedding-queue rows appear after a re-index batch (the
old behaviour -- this was the gap S5.G was meant to close); the rows fire
on every chunk (throttle is busted); the `(done)` row is missing at queue
drain (final-flush gate is busted).

## Notes

- The system-prompt bubble lives at the reserved dom_id 0; `clearChat`
  in `/reset` wipes it, and `LocusFrame::on_agent_session_reset` re-renders
  it from the (still byte-stable) assembly. The breakdown chip totals
  may shift if the workspace was reopened with a different LOCUS.md or
  with semantic search toggled on/off, since those change `MemoryStore`
  injection and `manifest_tokens` respectively.
- The hover-reveal `x` uses CSS opacity transitions, not display:none, so
  the X is always click-targetable by accessibility tools even when the
  user can't see it -- this is intentional.
- The embedding-throttle defaults (100 chunks / 5 s) live in
  `EmbeddingWorker::activity_chunks_per_event_` / `..._ms_per_event_`.
  No workspace-config knob in S5.G; if real usage shows noise, the knob
  graduates in a follow-up.
