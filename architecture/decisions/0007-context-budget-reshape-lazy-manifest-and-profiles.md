# 0007. Context budget reshape: lazy tool manifest + system-prompt profiles

- **Status**: Accepted
- **Date**: 2026-05-25
- **Supersedes (partially)**: nothing -- refines [0006](0006-tools-not-context-stuffing.md) by extending its "tools pull, not push" principle to the tool *manifest* itself.

## Context

The 2026-05-24 TestLocalVibe agentic-mode run ([findings.md](../../tests/ui_automation/output/agentic_TestLocalVibe/findings.md)) exposed a budget problem that the original [ADR-0006](0006-tools-not-context-stuffing.md) didn't anticipate. The principle "the agent retrieves its own context via tools" successfully keeps *file content* out of the prompt, but the **tool catalog** itself is still pushed every turn at full fidelity:

Measured on `qwen/qwen3.6-27b` (16384-token context):

| Component | Per-turn tokens | Source |
|---|---:|---|
| System prompt -- "## Available Tools" section | ~1000+ | [`system_prompt_assembly.cpp`](../../src/agent/system_prompt_assembly.cpp) walks `tools.all()` and prints `name`, `description`, every `ToolParam` with `type` + `description` |
| API request `tools: [...]` array | ~2854 | `tool_registry::build_schema_json()` -- the same content in JSON-schema form, mandatory for OpenAI-compatible tool dispatch |
| System prompt -- base rules + editing + shell + MSVC | ~700-1000 | Hard-coded prose in the same file |
| Workspace metadata / LOCUS.md / memory bank / format addendum | variable | Dynamic, generally small |

Total fixed overhead before the user message is composed: **~3577 tokens out of 16384** (the live log line `AgentCore: system prompt ~3577 tokens, context limit 16384`) **plus another 2854 tokens** in the API tools array, for a combined **~6431 tokens (39%) of fixed pre-budget per turn**.

The "Available Tools" prose section and the API tools array are duplicates: same 12 tool schemas in two different serialisations. Every turn pays for both.

On Qwen 3.6 27B during the TestLocalVibe run, the resulting ~10000 free tokens were not enough to fit (a) a moderately-sized user message, (b) the cumulative tool-call results across a multi-round turn, and (c) Qwen-style reasoning content. Round 3 of the original Task 2 attempt produced 666 KB of `<think>` content and never emitted a tool call before the watchdog had to abort. The same Task 2 with a `/no_think`-flavoured prompt completed in 183 s -- the model is capable; the budget pressure was the limiter.

Two cheaper local models (Gemma 4 E4B at 8K context, Qwen 3.5 9B at 32K) are visible in the same `/v1/models` listing on the test rig but neither would land Task 2 either: Gemma's 8K context can barely hold the overhead, and Qwen 3.5 9B's tool-call success drops sharply when its 2854-token manifest crowds out actual conversation history past 3-4 turns.

The principle from [ADR-0006](0006-tools-not-context-stuffing.md) -- "agent pulls, we don't push" -- already names the right shape. We just hadn't applied it to the manifest yet.

## Decision

Reshape the per-turn context budget along two independent axes:

### 1. Lazy tool manifest (S6.11)

The per-turn tool catalog (both the system-prompt prose section AND the API `tools: [...]` array) is trimmed to **summaries** by default: `name + one-line description`. Schemas for parameters are NOT included. A new built-in meta-tool `describe_tool(name)` returns the full JSON schema on demand. The agent calls `describe_tool` immediately before it needs to call an unfamiliar tool, the same way it already calls `get_file_outline` before `read_file`.

Concrete shape:

```text
## Available Tools (summaries -- call describe_tool('<name>') for the full schema)

- **read_file**: Read a slice of a workspace file by line range. Required for edit_file.
- **write_file**: Create a new file (or rewrite an existing one with overwrite=true).
- **edit_file**: Atomic byte-precise edit by old_string/new_string match. Requires prior read_file.
- **search**: Workspace search (modes: text / regex / symbols / semantic / hybrid).
- ...
```

API tools array carries the same one-line summary as each tool's `description` field, plus a permissive parameter schema (`{"type":"object","properties":{},"additionalProperties":true}`) so the LLM can syntactically emit a tool call even though it doesn't have the field names yet -- the dispatcher's existing validation catches missing required args and returns a clear error suggesting `describe_tool`.

The describe_tool meta-tool counts toward the manifest budget at roughly 80 t (single schema with one string parameter). Net per-turn manifest cost projected to fall from ~2854 t to ~400 t for the same 12 tools.

This is an **independent on/off setting** -- `tools.lazy_manifest` (default off in v1; the value gets revisited once telemetry from real users shows the round-trip cost of `describe_tool` calls in practice).

### 2. System-prompt profiles (S6.12)

The system prompt's prose sections (Rules / Editing and creating files / Running shell commands / Windows C++ pitfalls) are gated by a `system_prompt.profile` enum with three values:

- **`full`** (current behaviour, default): all sections, full prose, every example. ~700-1000 t of prose.
- **`compact`**: prose collapsed to load-bearing rules, no examples, no detailed parameter explanations. MSVC pitfalls section retained (it pays for itself within one build failure). ~300 t of prose.
- **`minimal`**: only the invariants a fine-tuned coding agent needs to be told explicitly: "Use tools to read files. Paginate. edit_file requires prior read_file. Be concise." ~150 t of prose.

Workspace metadata, LOCUS.md, memory bank, tools section (whose presence is driven by `tools.lazy_manifest`, not by the profile), and the tool-format addendum are **always rendered identically** across profiles -- they carry dynamic information or wire-format requirements that no profile should strip.

This is also an **independent setting** -- `system_prompt.profile` (default `full`).

### Composition

The two settings are orthogonal:

| `tools.lazy_manifest` | `system_prompt.profile` | Outcome |
|---|---|---|
| off | full | Today's behaviour (no change). |
| on | full | Lazy manifest; full prose. (~3-4 KB saved on tools, prose unchanged.) |
| off | compact | Full manifest; trimmed prose. (~500 t saved on prose, manifest unchanged.) |
| on | compact | Both savings; intended sweet spot for 16k-context models. |
| on | minimal | Maximum compression; for sub-16k contexts and fine-tuned agentic models. |

The matrix matters because users with capable models on 200K+ contexts may want lean tools but verbose system-prompt guidance (the model handles guidance well); users with small models may want a tight system prompt but full schemas in front of the model (the model isn't reliable enough at `describe_tool` followups yet). The two knobs separate cleanly.

### S4.F byte-stable invariant

Both knobs become part of the `SystemPromptAssembly` cache key. Toggling either knob mid-session invalidates the LM Studio / llama.cpp prefix cache for that session. Acceptable because:
- The toggles live in Settings, not in chat -- changing them is a deliberate user action.
- Within a single profile + lazy-manifest setting the prompt remains byte-stable across turns. The S4.F invariant becomes "byte-stable within a profile + manifest config", not "byte-stable always".
- `SystemPromptAssembly::hash()` already exists as the S4.F canary; the cache test in `tests/test_system_prompt.cpp` is extended to verify that hash IS stable when nothing toggles and CHANGES when either knob toggles.

## Consequences

**Wins**
- Recovers ~3-4K tokens of per-turn budget for the lazy-manifest+compact combination. On Qwen 27B at 16K context, that nearly doubles the usable conversation budget per turn.
- Aligns with [ADR-0006](0006-tools-not-context-stuffing.md): the tool catalog now follows the same "pull-on-demand" pattern as file content.
- Independent settings let the user dial each axis: model + context-size constraints map onto different combinations.
- The `describe_tool` meta-tool composes with the existing approval gate -- introspection actions are themselves observable.

**Costs**
- One extra round trip per first-use of a tool the agent doesn't already know the schema of, when `lazy_manifest` is on. Mitigations: (1) the tool descriptions even in summary form name the most common args inline ("**edit_file**: Atomic byte-precise edit by old_string/new_string match..."), (2) the agent typically reuses a small subset of tools repeatedly within a turn, so the schema-fetch cost amortizes, (3) the small-model robustness pass ([S6.10](../../roadmap/M6/S6.10-small-model-robustness.md)) Task E -- few-shot examples in tool descriptions -- pays double when descriptions are the only thing the model sees.
- The `minimal` profile is a footgun on weak models -- the deliberate trade is "fewer guardrails, you opt in". `compact` is the safer floor.
- S4.F prefix-cache breaks on knob change. Mitigated by Settings UX (group both knobs in one section so they're toggled together, not one at a time mid-session).
- Two new knobs to test: 6 cross-combinations (3 profiles x 2 manifest states), of which 2 are the realistic operational pairs (full+off = today; compact+on = small-model default). The remaining 4 get smoke coverage but not deep test matrices.

## Alternatives considered

- **Single combined "small-model mode" toggle that bundles both axes.** Rejected. Users with capable models on capacious contexts may want one axis but not the other (see the 200K-context case above). The two axes have different cost profiles and different reversibility -- collapsing them hides the actual choice the user is making.
- **Auto-gate by `query_model_info().context_length < 32K`.** Rejected. Model context length is metadata the user might be lying about (LM Studio reports the loaded model's training context, which may exceed what the user-set `n_ctx` will actually hold). An explicit Settings knob is more honest and easier to debug.
- **Drop the "Available Tools" prose section entirely (system prompt) and rely on the API tools array.** Tempting because the duplication is the most visible waste. Rejected because the Qwen-XML and Claude-XML tool-call formats ([S4.N](../../roadmap/M4/S4.N-stream-decoder.md)) do not consume the API tools array -- they need the schema visible in the prompt. The two listings serve different purposes per `tool_format`; they happen to overlap on `openai_json`. Lazy-manifest handles both paths uniformly by trimming both.
- **Bigger system-prompt prose with more examples for small models, instead of less.** This is what [S6.10](../../roadmap/M6/S6.10-small-model-robustness.md) Task E adds -- few-shot examples on specific tool descriptions. Composes cleanly with the profile system: `full` profile keeps examples expanded, `compact` strips them. The two stages can land in either order; together they let the user pick verbosity per axis.
- **Move the tools section entirely into the workspace's LOCUS.md.** Rejected. LOCUS.md is per-workspace, but tools are global -- duplicating across every workspace's LOCUS.md is worse than the current state, and a missing LOCUS.md would silently degrade tool discoverability.

## Notes

- This ADR governs two stage docs: [S6.11 (Lazy Tool Manifest)](../../roadmap/M6/S6.11-lazy-tool-manifest.md) and [S6.12 (System-Prompt Profiles)](../../roadmap/M6/S6.12-system-prompt-profiles.md). Each stage doc carries the implementation task list; this ADR carries the cross-cutting design rationale.
- Companion runtime work: [S6.13 (Reasoning Watchdog)](../../roadmap/M6/S6.13-reasoning-watchdog.md) caps misbehaviour during a single round; [S6.14 (Thinking Knob)](../../roadmap/M6/S6.14-thinking-knob.md) disables reasoning at the source. The four stages together turn "make Locus work on a 16K-context local model" from aspirational into operational.
- Reversing this decision is cheap: both knobs default to "off" / "full" which is current behaviour. The cost of the decision is mostly the new `describe_tool` tool surface and the test matrix.
