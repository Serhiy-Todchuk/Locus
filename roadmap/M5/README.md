# M5 -- Polish, UX & Performance

**Goal**: With the M4 engine in place, sand down the user-visible edges. UX refinements, performance optimization, robustness work, and quality-of-life touches that make the existing capability set feel fast, predictable, and discoverable. No big new subsystems -- this milestone is about the experience of using what we've built.

**Test workspaces unlocked**: WS1, WS2, WS3 (no new workspace requirements).

**Why before M6 (Connected)**: M6 multiplies surface area (remote frontends, web UI, VS Code shim, ZIM reader). Polishing the local experience first means each new frontend in M6 inherits a well-shaped foundation rather than papering over rough edges in three places.

## Stages (in execution order)

| Stage | Title | Theme |
|---|---|---|
| [S5.B](S5.B-terminal-panel.md) | Live Terminal Panel | UX |
| [S5.A](S5.A-capability-toggles.md) | Workspace Capability Toggles | UX / Tokens |

More stages will be added here as polish/UX/perf candidates surface. Letters are identity, not order -- listed top-down in execution order.

## Suggested order

S5.B (Terminal Panel) lands first: it is the most visible UX gap (long-running commands give the user no live feedback today) and reuses the existing `ProcessRegistry` + `RunCommandTool` plumbing without new agent-loop or LLM-side changes -- pure frontend work. S5.A (Capability Toggles) follows as the manifest-pruning win for short-context local LLMs. Both are independent; reorder if priorities shift.
