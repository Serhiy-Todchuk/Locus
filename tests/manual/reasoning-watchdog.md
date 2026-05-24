# Reasoning watchdog + Commit-now button (S6.13)

Per-round budget on the LLM's reasoning channel. Three Settings fields on Tool Approvals: `Reasoning max seconds`, `Reasoning max chars`, and the `Reasoning watchdog: auto-nudge` checkbox. Two action paths -- manual (a `Commit now` button surfaces in the chat footer next to Stop, user clicks to nudge) and automatic (`auto_nudge=true`, the agent self-nudges without user involvement). Hard cap: 2 nudges per turn before the turn aborts with "Agent appears stuck." Designed for the reasoning-heavy local models (Qwen 3.x with thinking on, DeepSeek-R1-distill, Qwen3.6-35b-a3b) where a single round can burn 25+ minutes of wall-clock without committing to a tool call.

Needs a reasoning model loaded in LM Studio (e.g. `qwen/qwen3.6-27b` with thinking enabled). On a non-reasoning model these tests don't trip because there's nothing to spin on.

## Test 1 -- auto-nudge cancels a runaway round and the turn finishes

1. Open Settings > Tool Approvals. Set `Reasoning max chars` to **5000** (tight on purpose so it trips quickly), tick `Reasoning watchdog: auto-nudge`, OK.
2. Send a deliberately vague prompt that invites reasoning: *"Think carefully about the trade-offs between BFS and DFS for solving a 9x9 sudoku. Don't write code, just reason."*
3. Within a minute or two -- as soon as the chat's "Thoughts" bubble crosses ~5K characters -- the log shows `AgentLoop: reasoning watchdog tripped (trigger=chars, chars=..., auto_nudge=true)` followed by `AgentCore: reasoning-watchdog nudge #1 injected (trigger=chars)`.
4. The chat shows a new user-style bubble `[Auto-nudge] Stop reasoning. Commit to a tool call now, or give a brief final answer if no tool is needed.` -- and the next round produces a short final answer instead of more thinking.
5. The activity log has a `User message (Reasoning watchdog nudge (#1, trigger=chars))` event so the trip is visible there too.

**Expected outcome**: the watchdog fires within ~one round's worth of reasoning past the threshold, the turn completes cleanly within 2 nudges. **Hard fail** if the turn aborts on the first nudge or the model keeps reasoning past the threshold without the watchdog firing.

## Test 2 -- manual Commit-now button surfaces, click ends the round

1. Settings: keep `Reasoning max chars` at 5000 but **untick** auto-nudge, OK.
2. Send the same vague reasoning prompt.
3. When the threshold trips, the status bar shows `Watchdog tripped (chars = N)` and a new `Commit now` button appears in the chat footer between Undo and Stop. The button was hidden before this turn.
4. Click `Commit now`. The same nudge sequence fires as Test 1 -- the LLM stream cancels, the synthetic user message gets appended, and the next round produces a short answer.
5. The button hides again when the agent goes idle (turn complete).
6. Send another reasoning prompt. The button reappears mid-turn when the threshold trips. Instead of clicking Commit now this time, click `Stop` -- the turn aborts entirely (`Turn cancelled.`) and the button also hides. Two different paths: Commit now keeps the turn alive, Stop kills it.

**Expected outcome**: the button is the manual equivalent of auto-nudge; Stop and Commit now are distinct verbs with distinct consequences.

## Test 3 -- hard cap aborts at 3rd would-fire

1. Settings: `Reasoning max chars` at a very tight **1500**, auto-nudge **on**, OK.
2. Send a vague reasoning prompt. The watchdog will trip every round because 1500 chars is well below what a reasoning model produces in a single round.
3. Watch the log -- `nudge #1`, `nudge #2` fire in order. On the 3rd would-fire, the log shows `AgentCore: 3 reasoning watchdog trips in one turn; aborting` and the chat shows the error: `Agent appears stuck (3 reasoning watchdog trips in one turn). Stopping. Adjust the prompt or raise agent.reasoning_max_chars / reasoning_max_seconds thresholds if the model genuinely needs more room.`
4. Turn ends. The `Commit now` button is hidden. A new user message starts a fresh turn with the nudge counter back at zero.

**Expected outcome**: the cap prevents an infinite nudge loop, the error message points at the relevant knobs, and the counter resets per-turn.
