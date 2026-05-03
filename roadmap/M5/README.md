# M5 -- Polish, UX & Performance

**Goal**: With the M4 engine in place, sand down the user-visible edges. UX refinements, performance optimization, robustness work, and quality-of-life touches that make the existing capability set feel fast, predictable, and discoverable. No big new subsystems -- this milestone is about the experience of using what we've built.

**Test workspaces unlocked**: WS1, WS2, WS3 (no new workspace requirements).

**Why before M6 (Connected)**: M6 multiplies surface area (remote frontends, web UI, VS Code shim, ZIM reader). Polishing the local experience first means each new frontend in M6 inherits a well-shaped foundation rather than papering over rough edges in three places.

## Stages

| Stage | Title | Theme |
|---|---|---|
| [S5.A](S5.A-capability-toggles.md) | Workspace Capability Toggles | UX / Tokens |

More stages will be added here as polish/UX/perf candidates surface.

## Suggested order

Stages in this milestone are mostly independent; pick the one whose pain is most acute at the moment. The first-open dialog work in S5.A is a natural early target because it directly cashes in the manifest-pruning opportunity captured during the Pi comparison.
