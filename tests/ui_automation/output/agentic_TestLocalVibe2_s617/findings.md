# S6.17 -- Acceptance Run (TestLocalVibe2 Pass 5, post-S6.17)

**Date**: 2026-05-27
**Model**: `qwen/qwen3.6-27b` @ LM Studio 127.0.0.1:1234
**Context**: 16384 tokens
**Workspace**: `D:/Projects/TestLocalVibe2/` (same fixture as the 2026-05-25 baseline)
**Workspace config** (key overrides):
- `agent.prompt_cost = "balanced"` (resolves to `lazy_tool_manifest=true` + `system_prompt_profile=compact`)
- `compaction.aggressiveness = "balanced"`
- `agent.reasoning_max_seconds = 180`, `reasoning_max_chars = 30000`, `reasoning_auto_nudge = true`
- `capabilities.semantic_search = false`, `code_aware_search = true`, `memory_bank = false`
- Write / edit / delete / run_command all auto-approved.

**Prompt** (verbatim, matching the 2026-05-25 baseline):
> big_module.cpp defines 10 subsystem classes. Pick the three you judge most cohesive / most worth extracting first, and split each into its own header + source pair under a new src/ directory. Use your own judgement on which three. Explain your reasoning in one sentence before you start.

---

## Outcome

| Axis | 2026-05-25 baseline | 2026-05-27 post-S6.17 |
|---|---|---|
| Wall-clock to turn-end | 35.6 min | 20.8 min (`elapsed_ms=1246715`) |
| Reached `reached=no` compactions | 3 of 4 (75%) | 0 of 1 (0%) |
| Compaction shrink ratio when fired | +37 tokens (no-op) | 12136 → 1339 tokens (**10:1**) |
| Files produced on disk | 0 | 0 |
| End-of-turn cause | 3-strike watchdog abort | 3-strike watchdog abort |

**Two of three acceptance criteria met:**

1. [x] **Compaction overhaul (Task B) is structurally correct.** The single compaction event that fired this run reached its target (target=9175, achieved=1339, ratio 10:1). The baseline's three consecutive no-op compactions never appeared in the new log. The fail-loud telemetry (warn -> error promotion on consecutive no-ops, `ActivityKind::compaction` no-op event) was wired in and present but never tripped because the pipeline didn't degrade.

2. [x] **Prompt-cost preset (Task H) shrinks fixed overhead.** Per-turn log line proves the resolution:
   ```
   AgentCore: system prompt ~682 tokens (profile=compact), context limit 16384
   Tool manifest: 14 tools, ~794 tokens, 3176 bytes (capabilities: +bg -sem +code -mem -web)
   ```
   Combined fixed overhead: ~1476 tokens (down from ~5300 t pre-S6.10 + S6.11 + S6.12). Per the S6.17 spec's prompt_cost = "balanced" mapping, this is the expected resolution.

3. [ ] **Pass 5 produces files on disk by turn-end.** The agent ran out of patience on the 3-strike watchdog (`reasoning_max_seconds=180` x 3 = ~9 min of watchdog-tracked time across 21 min of wall-clock); never reached the writing phase. **Not a compaction failure -- a model-speed / watchdog-tuning failure.**

---

## What the agent did (16 messages, ~5 rounds before abort)

1. **[3] get_file_outline** on big_module.cpp -> empty-state (the new S6.17 actionable message: "no symbols or headings found in the index", "Possible causes: ... language is not in the extractor table"). The model adapted on the spot and switched to `read_file`. Compare to 2026-05-25 where the silent `outline (0 entries)` line forced 4 dead rounds before the model gave up on outlines.
2. **[5/7/9/13/15] read_file** five times -- offset=1+length=150, offset=150+length=200, offset=349+length=200, offset=548+length=300, offset=847+length=400. The arg shape was correct (Task D's `reject_unknown_keys` plus the S6.10 examples drove the model to use the canonical `offset`+`length` form on the first try, no `lines:"100-300"` retries).
3. **[6/8/10/14/16] Tool results stripped** at compaction time by Layer 2 (strip_large_tool_bodies) -- consistent with the model having pulled 5 chunks totalling ~11000 tokens of file content before the cumulative-context overflow. Layer 2 firing meant compaction had work to do, which is the structural difference from the baseline.
4. Two `[Auto-nudge]` messages at [11] / [12] -- the watchdog tripped twice on slow reasoning between rounds and S6.17's wired-up `reasoning_auto_nudge=true` injected the synthetic user message both times. Compare to baseline's same pattern.
5. After compaction at 17:22:17 reduced context 12136 -> 1339, the model started a long reasoning round (~67000 bytes streamed over 2 min) which hit the 180s timer for the 3rd strike. Turn aborted.

---

## Findings (severity = original Pass 5 numbering preserved)

### F17 (H) -- **RESOLVED**. Compaction layers fail silently when there's nothing big to strip.

The 2026-05-25 baseline had three consecutive `reached=no` compactions adding 37 tokens each. This run had one compaction that achieved a 10:1 reduction (12136 -> 1339 tokens, reached=yes). The layer-escalation state (`AgentCore::compaction_no_op_streak_`) didn't even need to escalate because the first attempt already worked -- the layer matrix from the `balanced` aggressiveness preset (drop_redundant + strip_large + drop_old_reasoning + llm_summary) matched this conversation shape. The escalation code is wired and tested via unit tests; live verification waits for a conversation shape where Layer 1+2+3+6 still fall short.

### F18 (M) -- **NOT REPRODUCIBLE** in this run.

Sentinel composition wasn't exercised this run because only one compaction fired and it reached target on the first attempt. The progress-preserving sentinel work was deferred to a follow-up.

### F19 (M) -- **RESOLVED**. read_file `lines:` arg silently returned lines 1-100.

The model used `offset` + `length` correctly on all 5 read_file calls. No `lines:` shape attempted -- the few-shot examples (S6.10 Task E) plus the schema cues in `short_description()` (S6.17 Task I) appear to have steered the model to the canonical form. If the model HAD sent `lines:`, Task D's `reject_unknown_keys` would have caught it with the explicit "use offset+length instead" error.

### F20 (L) -- **STRUCTURALLY RESOLVED**. Agent chose `filter_output` for what `search regex` would have done.

`filter_output` is deleted (Task A). The model can no longer reach for it. This run the agent didn't reach for `search`-with-mode=regex either; it stuck with `read_file` paging, which on a 39 KB file took 5 rounds.

### F-NEW-1 (M) -- **Watchdog timer is too tight for Qwen 3.6 27B on this hardware.**

The 180s `reasoning_max_seconds` was the right starting value for fast hardware (Pi-tier H100), but on the user's local machine the model takes 90-120s per reasoning round on freshly-compacted context. Three consecutive slow rounds = abort. The new Task C timer thread is firing correctly (it's how the second nudge tripped despite a `chars=52` increment over the prior chunk callback), but the 180s wall-clock isn't permissive enough.

**Recommendation**: raise `reasoning_max_seconds` to 300-360 for this hardware class. Pre-existing knob, no code change needed.

### F-NEW-2 (L) -- Commit-phase skip (Task C.2) didn't suppress any trips in this run.

The condition `!tool_call_seen && text_chars > 100 && reasoning_chars == 0` requires reasoning_chars to be 0, but Qwen 3.6 27B always streams via the reasoning channel for tool-calling rounds. The skip correctly avoided false positives but didn't help on the actual failure mode here. **Not a regression** -- the skip is for a different shape (model committing to a final-answer text-only stream), not "model thinking through next tool call".

### F-NEW-3 (info) -- Telemetry is sharp.

The chunk-heartbeat lines fire at the documented 32-chunk / 10-second cadence; `LLM POST: sys_hash=...` lines confirm a stable prefix hash within the turn (cached=0 because LM Studio's prefix cache doesn't persist between requests at this temperature; that's a separate axis); the `Tool manifest: 14 tools, ~794 tokens` line proves Task H's preset resolution.

---

## Acceptance verdict

- **Task B compaction overhaul**: **PASS**. The Pass-5-shape failure mode that motivated the whole stage is fixed: compaction no longer silently grows context. The single live compaction in this run reached its target.
- **Task H prompt_cost preset**: **PASS**. Per-turn fixed cost shrank to ~1500t as designed.
- **Task D arg validation + Task I schema cues**: **PASS**. The model used canonical arg shapes on the first try across 5 read_file calls.
- **Task A filter_output removal**: **PASS**. The model didn't reach for it.
- **Task M get_file_outline empty-state**: **PASS**. The agent saw and adapted to the actionable message instead of getting stuck on the silent zero-entries line.
- **Files-on-disk smoke test**: **FAIL (different root cause)**. The agent didn't write files, but the failure mode has shifted from "compaction broke" to "watchdog too tight for this model on this hardware". The S6.17 work resolved the F17 finding it was scoped to resolve; the new failure mode is a tuning issue, not a structural one.

**Net**: the structural Pass-5 failure is fixed. A follow-up tuning pass (raise `reasoning_max_seconds`, possibly add per-model defaults via the existing S6.10 Task F preset matcher) would close the file-write gap.

---

## Artifacts

- Session JSON: `D:/Projects/TestLocalVibe2/.locus/sessions/2026-05-27_170458_aed7.json` (16 messages)
- Pre-compaction archive: `D:/Projects/TestLocalVibe2/.locus/sessions/2026-05-27_170424/history.before-compact-1.json`
- Locus log: `D:/Projects/TestLocalVibe2/.locus/locus.log` (filtered grep for `2026-05-27 17:` covers the full run)
- Workspace config: this run's `.locus/config.json` (preserved as `locus-config-seed.json` in this output dir)
