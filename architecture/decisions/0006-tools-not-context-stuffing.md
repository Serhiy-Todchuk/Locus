# 0006. Tools over context stuffing: agents pull, we don't push

- **Status**: Accepted
- **Date**: 2026-04-23

## Context

The dominant pattern in cloud AI coding assistants is **context stuffing**: at each turn,
the app scans the project, picks "relevant" files (by embedding similarity, recency,
heuristics), and injects their contents into the prompt before calling the model. Cursor,
Continue, and Aider all lean on some variant of this. It works well enough when the backend
is a 200K-token frontier model priced in cents per turn.

Locus is designed around different constraints:

- **Local inference on consumer hardware.** The v1 test model (Gemma 4 26B A4B) has an
  effective context around 8K–32K tokens at reasonable throughput. Every token fed in costs
  wall-clock time on the user's GPU.
- **Large workspaces from day one.** WS2 (Wikipedia ZIM dump) and WS3 (document library)
  are tens of gigabytes. No amount of clever pre-selection will fit them into a prompt.
  Even WS1 (this repo) has enough source to blow an 8K window if you naively pre-load
  "probably relevant" files.
- **Transparency as a hard requirement.** The user must see what data the agent is acting
  on. Silent context injection hides the most important variable in every response.
- **The workspace index already exists.** FTS5 keyword search, symbol search, semantic
  search, document outlines, directory listings — they are online and cheap. There is no
  reason the agent cannot query the same APIs the UI does.

The architectural principle was explicit from day one (see [vision.md](../../vision.md)
"Conservative Token Usage" and the first CLAUDE.md drafts). This ADR records *why* and
what we gave up, so later contributors do not drift back toward stuffing as "just a small
optimisation."

## Decision

**The agent retrieves its own context via tools. Locus never pre-loads files into the
prompt.**

Concrete rules, enforced by convention and by the tool roster:

1. **System prompt has no file contents.** It carries the workspace name, tool schemas,
   LOCUS.md (project-authored guidance), and optionally an *attached file* the user
   pinned via the GUI. The attached file is the single exception — the user asked for it
   and sees it in the UI.
2. **Every file read is a tool call the user can see.** `read_file`, `search_text`,
   `search_symbols`, `search_semantic`, `search_hybrid`, `list_directory`,
   `get_file_outline`. Each emits a `tool_call` activity event before executing and a
   `tool_result` event after. The approval gate applies.
3. **Tool results are summaries, not dumps.** `read_file` paginates (line ranges, not
   whole files). Search tools return ranked snippets with paths + line numbers, not full
   matched files. `list_directory` caps entries. The agent re-queries with a narrower
   filter when it needs more.
4. **The index is never written from the LLM.** Index writes are deterministic: the
   indexer and the background embedding worker. The agent can *query* the index (via
   tool results) but cannot corrupt it by misbehaving. This is also why index updates
   live outside `AgentCore` entirely — see the `IWorkspaceServices` split.
5. **Agent decides, user approves.** The LLM picks which tools to call. The user sees
   each one, can approve/modify/reject, and can set per-tool approval policies per
   workspace. There is no layer beneath the agent that tries to "help" by fetching data
   speculatively.

## Consequences

**Wins**
- Context stays tight: typical turn footprint is a few hundred tokens of search results
  plus the relevant slice of one or two files the agent explicitly asked for. Fits the
  local-model budget with room to spare.
- The user can audit every retrieval. If the agent answered wrong, the activity log shows
  exactly which files it looked at — the failure is either a bad query or a bad model
  response, never a hidden pre-injection.
- The same retrieval primitives power the UI search bar, the activity details pane, and
  the agent. One implementation, tested against itself.
- Scales to arbitrary workspace size. WS2's 30 GB Wikipedia dump is tractable because the
  agent never tries to "look at the whole workspace" — it searches, reads a page, cites
  the source. Adding a 100 GB archive changes nothing about the turn shape.
- Works with dumber models. Local LLMs sometimes return weaker responses than the cloud
  equivalent — but the failure mode is "agent doesn't know what to ask," not "agent
  drowns in context and hallucinates." The former is recoverable by the user; the latter
  is not.

**Costs**
- More round-trips per turn. A task that Cursor solves in one LLM call (because the file
  was already in context) can take Locus 2–4 rounds (`search_symbols` → `read_file` →
  answer, or similar). Cheap on local inference — no per-token cloud fee, only wall
  time — and each round is independently inspectable.
- The agent has to be prompted to *use* the tools effectively. A weak model that cannot
  plan queries underperforms compared to a strong model handed the file. The project
  mitigates this via LOCUS.md guidance, tool-call robustness (S4.N), and the option of
  routing harder turns to a stronger model (S4.Q).
- Some classes of question are harder — "what changed in this file over the last month?"
  (git-native features, S4.L) or "summarise the whole architecture" (needs breadth, which
  tools model as multiple queries). Addressed stage by stage; not a reason to abandon the
  principle.
- The agent can always still pull *too much* via a greedy `read_file` of a giant file or a
  wide `search_text` query. Pagination, truncation, and tool preview in the approval UI
  are the counter-pressure — not the absence of pull.

## Alternatives considered

- **Hybrid: auto-inject top-K search hits at the start of each turn, still allow tool
  calls.** Rejected. The auto-injected hits pollute the transcript whether the agent uses
  them or not, and every user sees the same "invisible" slice. We would inherit exactly
  the debugging problem we wanted to avoid.
- **RAG over the whole workspace baked into the system prompt.** Wrong primitive: the
  system prompt is long-lived and shared across turns, so per-turn retrieval would either
  leak or force expensive re-prompting. Tools are per-turn and per-call — the right grain.
- **Agent-written index updates.** Rejected as a correctness axiom. The LLM is allowed to
  read the workspace but not to mutate its metadata. Violating this opens the door to a
  corrupt index from a single bad tool response, with no easy way to recover.
- **Larger local models to dodge the token budget.** Fine as models improve, but the
  pattern this ADR records applies regardless: even a 200K-context local model is
  wall-clock-bounded on consumer hardware, and the transparency argument does not depend
  on model size.

## Notes

This is the single most load-bearing architectural principle in Locus, and it cascades into
every subsystem:

- The tool roster ([tool-protocol.md](../tool-protocol.md)) exists to give the agent pull
  primitives.
- The index subsystem ([workspace-index.md](../workspace-index.md)) exists so those pulls
  are cheap.
- The approval gate exists so every pull is observable.
- The `IWorkspaceServices` interface exists so tools have a minimum-surface handle on the
  workspace and cannot reach past it.
- Context management strategies ([overview.md](../overview.md) §"Context Management")
  exist to defend against the agent stuffing its *own* context by pulling too eagerly.

Reversing this decision would invalidate the goal statement in [vision.md](../../vision.md).
New ADRs may refine the pull primitives (e.g. streaming tool results, parallel calls,
reranking) but must not replace pull with silent push.
