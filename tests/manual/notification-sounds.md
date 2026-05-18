# Notification Sounds

Sound alerts for moments that need the user's attention. Each event maps to
a distinct Windows system-sound alias (PlaySound + `SND_ALIAS`), so the
exact waveform follows the user's Control Panel -> Sound -> Sounds theme.

Settings live under **Settings -> Notifications** -- four per-event toggles
plus an "Only when the Locus window isn't focused" gate.

Mapping:

| Event                  | Alias              | Default |
|------------------------|--------------------|---------|
| Tool approval pending  | SystemExclamation  | ON      |
| Ask-user question      | SystemQuestion     | ON      |
| Work done (turn end)   | SystemAsterisk     | OFF     |
| Compaction needed      | SystemHand         | ON      |

## Test 1: Round trip + per-event distinctness

1. Open Settings -> Notifications. Confirm five checkboxes appear with the
   defaults above (4 event toggles + "Only when not focused").
2. Enable all four event toggles. Uncheck "Only when the Locus window isn't
   focused". Click OK.
3. Send a prompt that the agent will answer with a tool call requiring
   approval (e.g. "run `echo hi`"). When the approval dialog opens, the
   SystemExclamation sound plays once. Approve.
4. After the tool finishes and the agent turn ends, the SystemAsterisk
   sound plays once.
5. Send a prompt that forces the agent to ask a clarifying question via the
   `ask_user` tool. When the question dialog opens, the SystemQuestion sound
   plays once. Answer or cancel.
6. Trigger compaction (chat large enough that the auto-compact threshold
   fires, or wait for the compaction dialog). The SystemHand sound plays
   once when the dialog opens.

Expected: four audibly distinct sounds, each fired exactly once at the
matching event. If the user has muted any of the four system sounds in
Control Panel, that one is silent -- the gate is per-alias, not Locus.

Hard fail: same alias firing twice for one event, or a sound firing
asynchronously well after the event (the call is `SND_ASYNC` so brief
overlap with the dialog appearing is normal -- "well after" means seconds).

## Test 2: Focus gate

1. Settings -> Notifications. Check "Only when the Locus window isn't
   focused". OK.
2. With Locus the foreground window, send a prompt that needs tool
   approval. No sound plays when the approval dialog opens.
3. Send a prompt that will take long enough to switch away (e.g. one that
   does multiple tool calls). While the agent is still working, click
   another app's window so Locus is no longer foreground.
4. When the next approval dialog opens (or when the turn ends, with that
   toggle enabled), the sound plays.

Expected: gate suppresses sounds while Locus is foreground, lets them
through while it's not. A dialog Locus itself owns (the approval modal,
ask-user, compaction) counts as Locus being foreground -- the gate checks
`GetAncestor(fg, GA_ROOTOWNER) == frame_hwnd`.

## Test 3: Per-event silencing

1. Settings -> Notifications. Uncheck only "Tool approval pending". Leave
   the rest enabled. Uncheck "Only when not focused". OK.
2. Trigger a tool approval. No sound.
3. Trigger an ask-user. SystemQuestion plays.
4. Let a turn end. SystemAsterisk plays.

Expected: per-event toggles independently silence their event without
affecting the others. Re-check the toggle and the sound returns next time.

## Test 4: Settings persistence

1. Settings -> Notifications. Flip a couple of toggles to non-default
   values. OK.
2. Close Locus. Re-open the same workspace.
3. Open Settings -> Notifications. The flipped values survive.
4. (Optional) Save as global defaults from the dialog footer. Open a fresh
   workspace -- it inherits the new defaults.

Expected: persisted in `.locus/config.json` under a `notifications` block;
inherited from `~/.locus/config.json` when a new workspace is opened.
