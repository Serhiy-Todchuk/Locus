# 0004. One app instance is bound to exactly one workspace folder

- **Status**: Accepted
- **Date**: 2026-04-23

## Context

The first design question Locus had to answer was: what is an "AI session" attached to?
Existing tools pick one of three answers:

- **Editor-scoped** (Copilot, Cursor): the agent lives inside an editor window; "workspace"
  means whatever folder that window has open, and a single agent instance can see several.
- **Project-scoped with multi-project UI** (most IDEs): one app, a project switcher, per-project
  state muxed through a common shell.
- **Conversation-scoped** (ChatGPT-style clients): the agent is a chat window, folders are a
  tool the agent happens to have.

Each option carries its own gravity. Editor-scoped means we live inside someone else's process
model. Multi-project means a correct answer to "which workspace does this tool call affect?"
at every single boundary — file watcher, index DB handle, `.locus/` config, tool approval
policy, LLM endpoint override, session store, LOCUS.md, token budget. Conversation-scoped
dissolves the workspace concept entirely and makes deterministic indexing impossible.

Two hard constraints ruled out the ambiguous choices early:

1. **The index is physical.** `.locus/index.db` + `.locus/vectors.db` + `efsw` watcher +
   background embedding worker are all keyed on "the folder." A multi-workspace process would
   have to run N of each, coordinate their writers, and reason about which one answers a
   given tool call. That is a distributed system, not a local assistant.
2. **Tool blast radius must be obvious.** `write_file("src/foo.cpp")` has to mean *exactly* one
   thing, resolved the same way by the approval UI, the executor, and the LOCUS.md check.
   Adding a workspace dimension to every tool call would be a correctness hazard, not a
   feature.

## Decision

**One Locus instance = one workspace folder, for its entire lifetime.** Users who want two
workspaces run two instances.

Concretely:

- The workspace path is a required argument to `Workspace` construction; it cannot be changed
  afterward. "Switch workspace" is implemented as launch-new-instance.
- `.locus/` lives inside the workspace root. Config, index DB, vectors DB, log file, session
  store — all workspace-local. There is no global Locus state directory for per-workspace data.
- The GUI runs a **single-instance check per workspace path** (mutex name derived from the
  canonicalised path). Opening the same folder twice focuses the existing window instead of
  starting a second daemon writing to the same `index.db`.
- All subsystems — `FileWatcher`, `Indexer`, `IndexQuery`, `ExtractorRegistry`, `AgentCore`,
  `ToolRegistry`, `IWorkspaceServices` — take the workspace as a constructor parameter and
  treat it as an invariant, not a field that can be mutated.

## Consequences

**Wins**
- Every tool call has exactly one workspace. No ambiguity, no dispatch layer.
- `.locus/` is self-contained and portable. Zip a workspace, move it to another machine, it
  just works. No global state to migrate.
- Single-writer per SQLite DB falls out naturally — two instances on the same folder are
  prevented by construction, not by locking.
- The `IWorkspaceServices` interface stays narrow: it represents *this* workspace, not a
  service locator across workspaces.
- Process-per-workspace gives OS-level isolation: a runaway indexer on folder A cannot stall
  agent responsiveness in folder B.

**Costs**
- A user who works in three folders runs three Locus processes. Each carries its own LLM
  client, embedding model memory, and tray icon. RAM cost is real but bounded; the embedding
  model is the dominant term and can be shared via the LLM backend if needed.
- No "open recent workspace" inside a single window — switching workspaces means closing and
  relaunching. The GUI smooths this with a Recent Workspaces menu that spawns a new instance.
- Cross-workspace operations (symbol lookup spanning two checkouts, for example) are not
  expressible. Accepted as out of scope — the agent is a local assistant, not a code search
  engine over everything you own.

## Alternatives considered

- **Multi-workspace in one process with a workspace selector.** Rejected: every tool call
  boundary and every subsystem would have to gain a workspace handle, and concurrent indexing
  would need cross-workspace coordination. High cost, no unlocked capability the
  process-per-workspace model doesn't already provide.
- **No workspace — workspace-is-whatever-the-agent-is-pointed-at-right-now.** Rejected: kills
  the persistent index, which is the entire basis for token-efficient retrieval over large
  folders (the project's founding use case).
- **Workspace stored globally in `%APPDATA%/Locus/<workspace-id>/`**, indirected by a stable
  id. Rejected: breaks zip-and-move portability and adds a synchronisation problem (rename the
  folder → global state goes stale). Workspace-local `.locus/` is simpler and survives renames.
- **Editor-embedded only (VS Code extension first).** Rejected as v1 — see
  [requirements.md](../../requirements.md). A standalone core keeps every frontend option
  open; a thin VS Code shim comes later as a client, not the host.

## Notes

This decision is load-bearing for essentially every other architectural choice: the split
index (0001), the `IWorkspaceServices` interface, the `.locus/` config layout, the
single-instance tray, and the multi-frontend registry (0005) all assume a stable one-to-one
mapping between process and workspace. Reversing this would not be a refactor; it would be
a rewrite.
