# M3 — Refactoring

**Goal**: Reshape the codebase so the next two milestones (Agent Quality, Connected) land without triggering god-classes, merge-conflict hotspots, or pointer-soup. Zero user-visible behavior change; every refactor is verified by existing tests + manual WS1 run.

**Why now**: M2 is functionally complete. M4 (old M3) adds ~22 stages and ~20 new tools — every one of them wants a hook into `AgentCore`, `ILLMClient`, `WorkspaceContext`, or `tools.cpp`. Reshape those seams *first* so each M4/M5 stage becomes a local edit instead of a god-class bloat.

**Scope discipline**: no new features, no behavior changes that a user could notice. Build + `locus.exe <ws> -verbose` + existing Catch2 suite pass after every stage.

## Stages

| Stage | File | Summary |
|---|---|---|
| S3.A ✔ | [agent-core-split](S3.A-agent-core-split.md) | Split 850-LOC `AgentCore` into loop / dispatcher / activity / budget collaborators |
| S3.B | [llm-client-split](S3.B-llm-client-split.md) | Split transport / stream-decoder / token-counter; prepare for multi-format + weak/strong |
| S3.C ✔ | [workspace-services](S3.C-workspace-services.md) | Replace `WorkspaceContext` raw-pointer struct with `IWorkspaceServices` interface |
| S3.D | [indexer-split](S3.D-indexer-split.md) | Extract Tree-sitter symbol rules + language registry + prepared-statement holder |
| S3.E | [tools-folder](S3.E-tools-folder.md) | Split `tools.cpp` into `src/tools/` subfolder by family |
| S3.F ✔ | [locus-frame-split](S3.F-locus-frame-split.md) | Extract menu controller; move file-watcher pump into Core |
| S3.G | [locus-session](S3.G-locus-session.md) | Bundle Workspace + LLM + ToolRegistry + AgentCore into `LocusSession` |
| S3.H | [src-layering](S3.H-src-layering.md) | Consistent `src/{core,agent,llm,index,tools,frontends,util}/` layout |
| S3.I | [threading-model](S3.I-threading-model.md) | Document threads, ownership, cross-thread invariants |
| S3.J | [slash-commands](S3.J-slash-commands.md) | Extract slash-command tokenizer + dispatcher into its own module |
| S3.K | [docs](S3.K-docs.md) | `agent-loop.md`, `threading-model.md`, ADR trail under `architecture/decisions/` |

## Suggested order

1. **S3.C** (workspace-services) — ~1 day; cheap; unblocks tool authors immediately.
2. **S3.F** (file-watcher pump into Core) — ~½ day; removes 3-way duplication before M5 adds a 4th.
3. **S3.A** (agent-core split) — ~3 days; the single biggest unblock for M4.
4. **S3.J** (slash commands) — falls out of S3.A naturally.
5. **S3.D**, **S3.E** — when the relevant M4 stage triggers them. Do not do both up-front.
6. **S3.B** — defer until M4 S4.N (tool-format robustness) or S4.Q (weak/strong) starts.
7. **S3.G** (LocusSession) — when M4 S4.B (checkpoint store) starts; the first new lifecycle subsystem is the natural moment.
8. **S3.H** (src layering) — emerges from S3.A/S3.D/S3.E; do not big-bang.
9. **S3.I**, **S3.K** (docs) — opportunistic, touch when adjacent code changes.

None of the stages are hard-blocked on another; ordering is about *opportunity cost* not dependency.

Update the Code Map in CLAUDE.md after each refactor.