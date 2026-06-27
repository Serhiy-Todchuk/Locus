# Coding with Small Local LLMs

Locus is not trying to out-code Claude Code or Cursor on raw capability. Those
tools assume a large, reliable, well-behaved cloud model. Locus assumes the
opposite: a small or mid-sized model running locally, with a tight context
window, on consumer hardware, that misformats tool calls, reasons in circles,
and forgets what it was doing after a context trim.

This document is the catalog of the **safety nets and small-LLM aids** layered
on top of that model to make it code reliably anyway. It is the "what's actually
in here" reference for an experienced user who wants to know what Locus does for
them without reading the per-stage roadmap docs.

Most of this is invisible when it works. The point of the catalog is so that when
something *does* surface -- a nudge in the chat, a footer chip, an aborted turn --
you know what fired and why.

Every entry tags the roadmap stage(s) that introduced it (e.g. `S6.13`); the full
rationale and tests live in [roadmap/M6/](roadmap/M6/). **PROPOSED** marks aids
that are designed but not yet shipped -- treat them as roadmap, not current
behaviour.

---

## Tool-call robustness

Small models routinely emit tool calls that are *almost* right. Rather than
dropping the call and burning a whole round on a retry, Locus repairs, coerces,
and steers.

- **Malformed-JSON tool-call repair** -- auto-repairs slightly broken tool calls
  (trailing comma, unquoted key, single quotes, unescaped newlines, a missing
  closing brace, or the call buried in prose) and unwraps wrapper shapes like
  `{"arguments": {...}}`, so the model doesn't waste a round re-emitting. (S6.10)
- **Tool-argument type coercion** -- when the model fills an argument with the
  wrong type (a quoted bool `"True"`, a number written as text `"2"`, an array
  smuggled inside a string), the value is coerced to what the tool expects
  instead of failing the call. (S6.19)
- **Stringified-array unwrap with single-quote repair** -- unwraps and repairs a
  list of edits or plan steps double-encoded as a string, even when it uses
  Python single-quotes instead of valid JSON. (S6.19)
- **Grammar-constrained tool-call decoding** -- where the server supports
  structured output (LM Studio / vLLM), generation is constrained to a JSON
  schema so malformed calls are physically impossible. Off / Best-effort / Strict
  modes; best-effort auto-retries unconstrained if the server rejects it, with
  JSON repair as the universal fallback. (S6.10)
- **edit_file single-edit shorthand** -- a top-level `old_string`/`new_string`
  pair is auto-wrapped into the array form the tool expects, so the model only
  has to produce the simplest plausible edit. (S6.17)
- **Case-tolerant enum arguments** -- enum-style string args (search mode, step
  status) are lowercased before matching, so `"TEXT"` or `"Done"` are accepted.
  (S6.17)
- **Reject unknown tool arguments with a fix-it hint** -- a wrong argument name
  (`lines` instead of `offset`+`length`, `cmd` instead of `command`) is rejected
  with the correct name plus an example, instead of silently running with
  defaults and producing wrong-looking results the model can't diagnose. (S6.17)
- **Applied-arguments echoed in result headers** -- content tools state the
  arguments actually applied (e.g. `lines 1-100 of 152 (offset=1 length=100)`)
  so the model can spot when its intent was silently replaced by a default and
  correct itself. (S6.17)
- **read_file alias rejection and recovery** -- an invented line-range form
  (`lines="100-152"`) is rejected with an error naming the correct offset/length
  shape, steering the model back to the supported call. (S6.18)
- **Unknown-tool closest-match suggestion** -- an invalid tool name returns the
  closest real tool (edit-distance match) plus the tools available that round;
  under the lazy manifest it also points the model at `describe_tool` for the
  schema. (S6.10, S6.11)
- **Missing-required-argument hint** -- a call omitting a required arg names the
  missing arg and tells the model to call `describe_tool` for the full schema,
  giving a concrete recovery action. (S6.11)
- **Self-correcting tool-error messages** -- a tool that fails on a wrong-typed
  arg returns the tool's full argument schema, so the model sees the expected
  shape and fixes it next round rather than re-sending the broken call. (S6.20)
- **Per-endpoint tool-call format override** -- each endpoint can pin the
  tool-call dialect (OpenAI JSON, Qwen-XML, Claude-XML, or auto-detect) to avoid
  dropped/garbled calls from a provider that speaks a non-standard format.
  (S6.16)
- **Actionable empty-state for file outline** -- an empty outline checks the
  filesystem and explains why (missing, too large, recognition/timing) with next
  steps, instead of a silent `0 entries`. (S6.17)
- **PROPOSED -- Readable malformed-tool-call error** -- a dropped call names the
  tool, the reason (bad JSON vs suspected truncation), and a size hint, so a
  watching user can diagnose it without opening the trace log. (S6.21)
- **PROPOSED -- edit_file self-healing error hints** -- a failed exact-string
  edit (anchor matches several places / not found) appends the concrete remedy:
  use `replace_all`, add surrounding context, or re-read and switch to a full
  overwrite. (S6.21)

---

## Context budget & prompt shaping

A 16k-context model can lose a quarter of its window to the tool catalog and
system prompt before the user even types. These knobs reclaim that budget, and
make the trade-offs visible.

### Tool manifest

- **Lazy tool manifest** -- sends only each tool's name and a one-line summary
  per turn instead of full parameter specs, freeing a large chunk of context
  (measured ~2854 -> ~400 tokens across the built-in tools). User-toggleable
  per workspace. (S6.11)
- **describe_tool meta-tool** -- the model calls this to pull the full schema for
  one tool only when it needs it; the trade-off that makes the lazy manifest
  safe. Always kept fully described even when the rest of the list is collapsed.
  (S6.11, S6.17)
- **Short tool descriptions** -- the lazy manifest uses a curated under-60-char
  one-liner per tool instead of full prose, collapsing the tool section's cost
  ~90%. (S6.10)
- **Schema cues inline in tool one-liners** -- each short description spells out
  its required args and enum values inline (e.g.
  `read_file(path, offset=1, length=100)`) so the model gets the shape right
  without spending a round fetching the full schema. (S6.17)
- **Worked examples in tool descriptions** -- high-leverage tools carry a
  concrete example call (small models follow exact patterns over abstract prose);
  the examples only cost tokens when the full description is fetched. (S6.10)
- **Drop duplicated tool listing for OpenAI-format models** -- when the model
  consumes the native API tools array, the redundant prose "Available Tools"
  section is replaced by a one-line pointer (kept for XML-format models that need
  it). Combined with short descriptions, ~76% cut in per-turn fixed overhead.
  (S6.10)

### System prompt

- **System-prompt profiles (Full / Compact / Minimal)** -- trades built-in
  guidance against context: Full (~700-1000t), Compact (~300t), Minimal (~80t,
  a five-line prompt). On a capable model the long guidance is redundant. (S6.12)
- **Load-bearing guidance preserved in Compact** -- when trimming, the rules that
  actually steer a small model are kept: don't guess file contents (use tools),
  paginate large reads, prefer in-place edits over full rewrites, read-before-edit,
  a couple of Windows/MSVC build gotchas. (S6.12)
- **Prefix-cache-aware behaviour** -- within a chosen profile the system prompt
  is byte-identical across turns so the local server's prompt cache survives; a
  profile switch resets it once (called out as a one-time cost, not a silent
  regression). (S6.12)
- **prompt_cost preset (Minimal / Balanced / Verbose / Default-auto)** -- bundles
  the interacting context-saving knobs (lazy manifest, prompt verbosity, tool
  format) into one Settings > Agent dropdown and auto-picks a combination based
  on the model's context size. (S6.12, S6.17, S6.18)

### Reasoning-token control

- **Strip past-turn thinking from the payload** -- drops earlier turns' reasoning
  (5-20k tokens each for reasoning models) from what is resent, keeping the
  in-flight reasoning when mid-decision; the full reasoning stays visible in chat
  and saved sessions. (S6.10)
- **PROPOSED -- Thinking ON/OFF/Auto toggle** -- forces the model's reasoning
  mode on/off (or Auto) per workspace, stopping a small model from burning a
  round on chain-of-thought that never produces a tool call. Trigger case: a Qwen
  model streamed 666 KB of reasoning and timed out; the same task with thinking
  off finished in ~1/10th the time. The root-cause companion to the reasoning
  watchdog. Translates one On/Off choice into the right mechanism per family
  (newer Qwen template flag, older Qwen `/no_think` suffix, o1/DeepSeek-R1
  reasoning-effort). (S6.14)

### Compaction

- **Escalating compaction (no-op recovery)** -- if a trim pass fails to free
  enough room, progressively more aggressive strategies fire in a fixed order
  until the conversation fits, instead of giving up and trimming nothing while
  context keeps growing. (S6.17)
- **Count-based trimming for many small results** -- trims when too many tool
  results have accumulated (dropping the older half) regardless of each one's
  size, covering tasks that produce many already-trimmed small outputs. (S6.17)
- **Compaction aggressiveness preset (Gentle / Balanced / Aggressive / Custom)**
  -- one Settings > Agent setting replaces five granular toggles; Balanced is the
  default. Power users can still hand-tune via config. (S6.17, S6.18)
- **Progress-preserving compaction summary** -- on compaction the agent writes a
  short LLM-generated progress summary into the surviving breadcrumb instead of a
  generic placeholder, keeping a small model oriented about what it already did.
  (S6.18)
- **Last-answer pinning** -- compaction pins the most recent real assistant
  answer (text, no pending tool calls) so it is never dropped, avoiding the
  failure where compaction erases the very thing the user just asked about.
  (S6.18)
- **Hard-trim backstop** -- if layered compaction still can't fit, a last-resort
  pass drops oldest turn pairs one at a time while always keeping the system
  prompt and first user message, guaranteeing the window never overflows. (S6.18)
- **PROPOSED -- Compaction breadcrumb for build loops** -- after a hard trim, if
  a live build-error signature exists, prepends a one-line reminder of the last
  error and the file being edited so a small-context model resumes the same fix
  instead of re-deriving it. Pure-chat sessions unaffected. (S6.21)

---

## Convergence & anti-loop safety nets

A small model that can't make progress will keep trying the same broken thing.
These detect the loop and either steer the model out of it or stop the turn
before it wastes more time. All nudges share one capped budget: **at most two
corrective hints per user request, then the turn aborts** with a clear "agent
appears stuck" message rather than nudging endlessly. (S6.13, S6.20)

- **Empty-response correction** -- a turn with no text and no tool call gets a
  corrective message telling the model to respond with text or a tool call,
  instead of the agent stalling silently. (S6.10)
- **Repeated-tool-call correction** -- detects the same tool called with the same
  args back-to-back (a no-progress loop) and injects a message to try a different
  approach, args, or tool. (S6.10)
- **Reasoning watchdog** -- caps how long a model can sit "thinking out loud"
  without acting, via three independent ceilings (elapsed seconds, reasoning
  characters, rounds without a tool call); the first crossed trips it. Rescues
  reasoning-heavy models (Qwen-thinking, DeepSeek-R1 distills) that otherwise
  spiral for 10-25 minutes and never commit. Off by default, opt-in, persists
  across relaunch. (S6.13)
- **Commit-now nudge** -- on trip, injects a short instruction to stop reasoning
  and call a tool or give a brief final answer now, *without* throwing away the
  turn (unlike Stop). In manual mode a "Commit now" button appears next to Stop
  only while stuck; in auto mode (unattended/agentic runs) the nudge fires by
  itself the moment a limit is crossed. (S6.13)
- **Reasoning-channel productive-progress skip** -- while a model is still
  visibly making progress in its hidden reasoning channel, the character-budget
  cutoff is suppressed so reasoning-heavy models (e.g. Qwen3 27B streaming
  tool-call deliberation through reasoning) aren't killed mid-thought. The
  wall-clock cutoff is evaluated independently, so a genuinely stuck/silent
  stream still aborts on seconds. (S6.18)
- **Timer-driven watchdog** -- a 5s timer checks independently of incoming token
  chunks, so a stalled stream with long silent gaps between chunks is still
  caught. (S6.17)
- **Don't abort during final text-only commit** -- a model slowly writing its
  final answer (text only, no tool calls, no separate reasoning) is detected and
  spared by the watchdog. (S6.17)
- **"Stuck on the same build error" detector** -- notices the model hitting the
  same compiler/linker error round after round even though each attempt is a
  *different* code change, steers it once ("stop rewriting, read the failing
  lines, make a minimal edit"), and aborts if it still loops. Catches a thrash
  the repeated-tool-call guard misses (the calls differ, only the error stays the
  same). (S6.20)
- **Error-signature normalization** -- distills a build error to a fingerprint
  that ignores line/column numbers, paths, hex addresses, counts, and diagnostic
  order while keeping error codes distinct, so "same failure" is recognized
  across rewrites. Warnings are deliberately ignored. (S6.20)
- **Edit-over-rewrite nudge for large files** -- when a wholesale rewrite of a
  large file (~16 KB / 400+ lines) then fails to build, a one-time hint says to
  switch to targeted edits, cutting token waste and often heading off the
  stuck-error loop. Narrow on purpose -- a large rewrite that compiles is left
  alone. (S6.20)
- **Agent mode and in-flight plan survive a reopen** -- closing in Plan/Execute
  mode with a live multi-step plan reopens into the same mode with the plan
  bubble intact, keeping a long planned task resumable. (S6.15)
- **PROPOSED -- All-tool-calls-dropped nudge then abort** -- a round where zero
  calls landed and at least one was dropped as unparseable gets one corrective
  steering message (re-send a properly quoted JSON object) and aborts as "stuck"
  if it recurs; a round with one valid call resets the count. Closes a silent
  loop the result-based detectors above can't see, because a zero-result round
  produces nothing for them to key on. (S6.21)
- **PROPOSED -- edit_file ambiguity loop nudge** -- if the same "matches N
  locations" / "not found" edit error repeats within a turn, the escape-hatch
  hint is injected once, catching small models that loop re-reading and
  re-guessing the anchor. (S6.21)

---

## Endpoint & transport resilience

A local server stalls; a free hosted tier rate-limits. The agent shouldn't throw
away a multi-round build because of a transient hiccup -- and where a small model
genuinely can't do a turn, you should be able to hand that one turn to a bigger
model without losing context.

- **Auto-detect loaded model and apply preset** -- at startup, matches the loaded
  model id (Qwen3-Coder, Qwen, Gemma, DeepSeek-R1, Llama 3.x) and applies
  known-good tool-call format and sampler defaults; wrong settings can drop
  tool-call success 30-50%. An unknown model leaves config untouched. (S6.10)
- **Per-model sampler defaults** -- each family preset carries tuned sampling
  knobs (temperature, top_p, top_k, min_p, repetition penalty) from the model's
  card; a "Reset samplers to preset" button re-baselines just the sampler block.
  (S6.10)
- **Mid-session endpoint switching** -- keep several LLM sources configured and
  flip between them from a chat-footer dropdown without editing config or
  reopening. The payoff for small-model reliability: run a cheap local model for
  retrieval/outline rounds, then switch to a far larger hosted model (e.g. a 480B
  coder) for the actual code-writing turn. Switching keeps the full conversation
  and applies on the next user message; a switch requested mid-turn is deferred
  to end-of-turn. (S6.16)
- **Per-endpoint preset + context-limit refresh on switch** -- switching
  re-queries the new server and re-applies known-good defaults for that model
  family, and updates the context meter to the new model's real window so you
  don't silently overrun a smaller one. (S6.16)
- **Hosted-provider authentication** -- API-key (Bearer) auth and arbitrary extra
  headers so paid/hosted providers (NVIDIA, OpenAI, OpenRouter, Claude-via-proxy)
  work, broadening the fallback pool beyond a single local server. Keys live in
  one global file (never in a workspace, never git-committed), are never logged,
  and are masked in the UI. (S6.16)
- **Base-URL join guard** -- joins the base URL and path so a provider-documented
  `/v1` suffix never doubles to `.../v1/v1/...` and 404s. (S6.16)
- **Transient endpoint retry with backoff** -- on a hosted hiccup (429, 5xx,
  request timeout, or a successful-but-empty response after a long queue wait)
  retries up to 5 attempts with growing waits instead of discarding a whole
  multi-round build; 429 waits longer, `Retry-After` is honored. Genuine
  non-transient failures (bad request, auth, connection refused, cancel) are not
  retried. (S6.19)
- **Retry / latency status in the chat footer** -- on a transient failure and
  silent backoff, the footer shows a live status like `HTTP 429 (rate limited)
  -- retrying 2/5 in 8s` so the user can tell a recovering endpoint from a stuck
  agent. Previously these multi-minute waits were completely silent. (S6.20)

---

## Retrieval & navigation

Retrieval is how a small model avoids loading whole files into a tiny context.
These keep what the agent searches accurate and the tool choices unambiguous.

- **Search split into one tool per mode** -- the multi-mode search tool became
  `search_text` / `search_regex` / `search_symbols` / `search_semantic` /
  `search_ast`. Removing the mode-discriminator step means the model picks a
  clearly-named tool instead of guessing a mode arg (`search_regex` reads as
  "grep"). A one-version shim rewrites old saved sessions. (S6.17)
- **Removed redundant filter_output tool** -- dropped a tool overlapping with
  inline command-output filtering and regex search that models kept misusing as
  grep-over-file; fewer near-duplicate tools means fewer wrong choices and a
  smaller manifest. (S6.17)
- **Per-tool search-result granularity in descriptions** -- each search tool
  states what one result row represents (per file / per match / per symbol /
  per chunk / per AST node) so the model picks the right tool and reads counts
  correctly. (S6.18)
- **Mid-turn index refresh after the agent edits files** -- on a write/delete the
  index is flushed immediately so the agent's own changes are visible to its next
  read (outline, symbol search, listing), fixing the case where a just-written
  file looked empty/unindexed in the same turn. (S6.17)
- **Case-insensitive directory annotation fallback** -- listing a folder falls
  back to a case-insensitive match before declaring a file unindexed, so a
  Windows rename/case-change doesn't produce misleading `[unindexed]` tags the
  model might act on. (S6.18)
- **Embedding overwrite on chunk-id reuse** -- when a file is rewritten faster
  than the background embedder keeps up, the new embedding overwrites the
  colliding row instead of being silently dropped, so semantic search reflects
  current code rather than a stale version. (S6.18)

---

## Transparency & restore

The small-model story is also the visibility story: when a safety net fires, the
user should see it. Recovery that happens invisibly looks like the model
"magically" getting better (or worse).

- **Anti-truncation guard on file writes** -- before writing/editing, scans the
  content tail for elision markers small models leave when they cut a file short
  (`// rest of the code`, `// ... existing code ...`, `# TODO: implement` at EOF)
  and refuses the write. High-precision phrase list so real code never trips it.
  (S6.10)
- **Crash-proof tool preview backstop** -- rendering a pending tool call for
  approval can no longer crash the GUI on a malformed argument the coercers
  didn't anticipate; the preview falls back safely. Closes a single-bad-token
  crash class. (S6.19)
- **Per-correction visibility events** -- every auto-correction and blocked
  truncation emits an activity-log event with kind and summary. (S6.10)
- **Watchdog activity log events** -- every trip and nudge records which limit
  fired, the value, the threshold, and the nudge number. (S6.13)
- **Auto-corrections chip** -- a chat-footer chip shows a running count of
  quality-monitor corrections injected this session, a passive at-a-glance signal
  the safety net is active. (S6.18)
- **No-op compaction visibility** -- the compacted-count chip surfaces how many
  compaction attempts failed to reduce tokens, with a tooltip, making a saturated
  context window visible rather than a mysterious model degradation. A failed
  trim pass also logs a warning (error if twice in a row). (S6.17, S6.18)
- **Full conversation save/restore including hidden reasoning** -- reopening a
  saved chat brings back collapsed "Thinking" bubbles (labeled "Thinking
  (restored)"), not just final answers, so a long task resumes with earlier
  chain-of-thought intact. Tool-call previews are reconstructed too, even for
  tool-only turns. (S6.15)
- **Opt-in persistent activity timeline** -- an optional append-only on-disk log
  records the per-turn timeline (tool calls, results, indexing, prompt assembly)
  so the Activity panel rebuilds it after a close; a durable audit trail for
  multi-day sessions. Off by default; an interrupted write loses only the last
  partial entry. (S6.15)
- **PROPOSED -- Unverified-success tripwire** -- when the assistant ends a turn
  claiming a verifiable outcome (it builds/compiles, screenshot created, exe runs)
  but ran no confirming command or file check that round, a non-blocking note
  flags the unverified claim. A visibility warning for the watching user, kept
  conservative to avoid false alarms on legitimate summaries. (S6.21)

---

## How these fit together

A typical hard turn on a 16k-context local model exercises several of these at
once. The model picks a search tool by its clear name and inline arg cues
(retrieval); its slightly-broken tool call is repaired or its mistyped arg
coerced (tool-call robustness); the lazy manifest and a Compact prompt profile
left it enough budget to read the result (context budget); when it starts looping
on the same compiler error, the stuck-error detector nudges it once toward a
minimal edit (convergence); if the endpoint rate-limits, the turn waits and
retries instead of dying (transport); and a footer chip tells you a correction
fired (transparency). None of it competes with a frontier cloud model -- it makes
a model that *would* otherwise fall over get the job done.

For the design rationale, failure-mode write-ups, and tests behind each aid, see
the stage docs in [roadmap/M6/](roadmap/M6/) (S6.10 through S6.21) and the
cross-cutting key invariants in [CLAUDE.md](CLAUDE.md).
