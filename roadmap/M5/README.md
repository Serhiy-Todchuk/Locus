# M5 -- Polish, UX & Performance

**Goal**: With the M4 engine in place, sand down the user-visible edges. UX refinements, performance optimization, robustness work, and quality-of-life touches that make the existing capability set feel fast, predictable, and discoverable. No big new subsystems -- this milestone is about the experience of using what we've built.

**Test workspaces unlocked**: WS1, WS2, WS3 (no new workspace requirements).

**Why before M6 (Connected)**: M6 multiplies surface area (remote frontends, web UI, VS Code shim, ZIM reader). Polishing the local experience first means each new frontend in M6 inherits a well-shaped foundation rather than papering over rough edges in three places.

## Stages (in execution order)

| Stage | Title | Theme |
|---|---|---|
| [S5.B](S5.B-terminal-panel.md) | Live Terminal Panel | UX |
| [S5.A](S5.A-capability-toggles.md) | Workspace Capability Toggles | UX / Tokens |
| [S5.C](S5.C-inline-diffs-in-chat.md) | Inline Code Diffs in Chat | UX |
| [S5.D](S5.D-reserve-and-token-xray.md) | Reserve Enforcement + Token-Cost X-Ray | Context |
| [S5.E](S5.E-pin-and-tag.md) | Per-Message Pin / Tag System | Context |
| [S5.F](S5.F-compaction-v2.md) | Compaction v2 (Toolkit + Auto + History Archive) | Context |
| [S5.G](S5.G-chat-activity-restructure.md) | Chat / Activity Restructure | UX |
| [S5.H](S5.H-edit-delete-branch.md) | Per-Message Edit / Delete + Branch & Rewind | Context |
| [S5.I](S5.I-tabs-and-sessions.md) | Multi-Tab Conversations + Session Menu Redesign | UX |
| [S5.Z](S5.Z-misc-gaps.md) | Miscellaneous Smaller Gaps | Mixed |

More stages will be added here as polish/UX/perf candidates surface. Letters are identity, not order -- listed top-down in execution order. S5.Z is the misc bucket at the end (mirrors [S4.V](../M4/S4.V-misc-gaps.md)) and is expected to grow.

### Context-management cluster (S5.D - S5.H)

Five stages forming a coherent context-management overhaul, designed for short-context local LLMs (32k typical for coding work). Dependency order:

1. **S5.D** lands first -- reserve enforcement + per-message token visibility are the foundation; everything else depends on them.
2. **S5.E** opens user control over what survives compaction (3-state pin button per message).
3. **S5.F** ships the composable compaction toolkit (six layers run as a pipeline), threshold bands, auto-compaction for unattended sessions, and the history-archive backup chain.
4. **S5.G** restructures chat to be the LLM's view (system prompt visible, per-message token / pin controls integrated) and activity log to be pure system audit (gains missing indexing/embedding events).
5. **S5.H** adds the surgical operations -- edit, delete, rewind, branch, restore-from-archive.

Each stage is shippable independently and the user-visible value compounds.

## Suggested order

S5.B (Terminal Panel) lands first: it is the most visible UX gap (long-running commands give the user no live feedback today) and reuses the existing `ProcessRegistry` + `RunCommandTool` plumbing without new agent-loop or LLM-side changes -- pure frontend work. S5.A (Capability Toggles) follows as the manifest-pruning win for short-context local LLMs. Both are independent; reorder if priorities shift.
