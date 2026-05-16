# Compaction v2 (S5.F)

Three tests covering the new layer cascade, the manual dialog, and the chained history archive. The auto-compact band fires automatically when usage crosses `compaction.auto_threshold` (default 85%) of the effective context limit; manual compaction goes through the rebuilt Compact dialog or the `/compact [instructions]` slash command.

## Test 1: manual compaction via dialog

1. Open the workspace against an LLM (e.g. LM Studio with any tool-trained model). Drive a few turns that mix `read_file`, `list_directory`, and a redundant second `read_file` of the same path so the history accumulates duplicate tool results.
2. Open the Compact dialog (Edit > Compact Context or status-bar shortcut). The body shows five per-layer checkboxes (drop redundant tool results, strip large tool bodies, drop reasoning from older turns, drop oldest turn pairs, LLM summary) plus per-layer knobs (strip threshold tokens, older-than turns, keep-recent turns, summary max tokens). Below that: a "Custom summary instructions" multiline box and a Preview pane showing `Before:`, `After: ~N (M%)`, `Freed: ~K`, and a per-layer breakdown.
3. Toggle a checkbox -- the preview recomputes inline (no LLM call yet). Drag `Strip threshold` down to 100 to confirm the Layer 2 "freed" estimate jumps up. Turn Layer 5 on; preview shows N drop-spans + estimated freed tokens.
4. Type "preserve all file paths and test commands" into the custom-instructions box. Click Compact.
5. Watch the status bar: "Context compaction queued". On the agent thread, the archive lands at `.locus/sessions/<session_id>/history.before-compact-1.json` (a verbatim JSON of the pre-compaction history). The context meter recomputes; a token chip in chat reports the result ("Compaction applied: freed ~N tokens ..."). The Activity log gets a `compaction` row with the per-layer detail.
6. Open the archive file in a text editor -- the `messages` array contains the original, full conversation. The current live `<session_id>.json` (saved when you click File > Save Session) is the compacted view; the archive is the recovery point.

**Expected outcome**: dialog reflects layers + knobs; preview is live; archive is byte-identical to the pre-compaction state; custom instructions appear in the layer 6 system message the agent sends.

## Test 2: /compact slash command

1. In the same workspace, type `/compact` (no args). Same cascade fires as auto-compact: Layers 1+2+3+6, layer 5 off. Status bar / chat shows the freed-tokens summary.
2. Now type `/compact focus only on the test names`. The override appears in the Activity row's detail ("custom instructions: focus only on the test names"). Layer 6's summary should bias toward test-name context.
3. After three `/compact` invocations, the archive directory contains three `history.before-compact-N.json` files (counters 1, 2, 3). Run a fourth and a fifth; the fifth shows counter 5. Run a sixth; the oldest (counter 1) is GC'd because `archive_keep_count` defaults to 5.
4. Edit `.locus/config.json`: set `"compaction.archive_keep_count": 2`. Trigger another `/compact`. Now only counters 5, 6 (or the newest two by counter) remain; older snapshots are GC'd on the next compaction.

**Expected outcome**: per-run override never persists to `compaction.custom_summary_instructions`; GC respects `archive_keep_count`; filenames are stable.

## Test 3: auto-compact + heuristics

1. Set up: in `.locus/config.json`, lower `compaction.auto_threshold` to `0.10` and `compaction.warn_threshold` to `0.05`. Restart the workspace.
2. Drive a single big turn that exceeds 10% of the effective context limit (e.g. attach a large file and ask the agent to refactor it, or paste a multi-K-token stack trace). Between LLM rounds the agent loop crosses `auto_threshold` and the cascade fires automatically: log line `auto-compact firing (...% of effective_limit, threshold 10%)`, archive snapshot, then `pipeline compaction <before> -> <after>`. The chat shows the freed-tokens summary card; the user wasn't prompted for a modal.
3. Verify preservation heuristics survive the cascade:
   - The first user message of the session is still in the post-compaction history (open the live `<session_id>.json` and grep for it).
   - Short user messages (your typed prompts) survive even when a turn is dropped around them.
   - A user message containing `[Attached file: ...]` survives.
   - The two most recent turns (default `keep_recent_turns: 3` -- per stage doc -- counts user/assistant pairs) are intact.
4. Open the chat: the synthetic user-role footnote reads `[Earlier context compacted; full pre-compaction history archived at .locus/sessions/<id>/history.before-compact-N.json]` and lives right after the system prompt (not inside it -- the system message stays byte-stable for S4.F KV-cache reasons; verify by hashing the leading system content before and after compaction).
5. Disable auto-compact: set `compaction.auto_enabled: false`. Drive the same big turn -- the warn band still updates the footer chip but no automatic cascade fires; instead, you must click Compact or run `/compact` yourself.

**Expected outcome**: the cascade fires without a modal, the archive trail is present and walkable, the auto-preservation heuristics keep the load-bearing parts of the conversation, and the S4.F system-prompt invariant survives compaction.
