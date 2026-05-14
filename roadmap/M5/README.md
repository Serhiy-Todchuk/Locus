# M5 -- Polish, UX & Performance

**Goal**: With the M4 engine in place, sand down the user-visible edges. UX refinements, performance optimization, robustness work, and quality-of-life touches that make the existing capability set feel fast, predictable, and discoverable. No big new subsystems -- this milestone is about the experience of using what we've built.

**Test workspaces unlocked**: WS1, WS2, WS3 (no new workspace requirements).

**Why before M6 (Connected)**: M6 multiplies surface area (remote frontends, web UI, VS Code shim, ZIM reader). Polishing the local experience first means each new frontend in M6 inherits a well-shaped foundation rather than papering over rough edges in three places.

## Stages (in execution order)

| Stage | Title | Theme |
|---|---|---|
| [S5.L](S5.L-ui-automation-driver.md) ✔ | UI Automation Test Driver | Testing / Infra |
| [S5.B](S5.B-terminal-panel.md) ✔ | Live Terminal Panel | UX |
| [S5.A](S5.A-capability-toggles.md) ✔ | Workspace Capability Toggles | UX / Tokens |
| [S5.C](S5.C-inline-diffs-in-chat.md) | Inline Code Diffs in Chat | UX |
| [S5.J](S5.J-llm-context-refactor.md) | LLMContext Class Refactor | Refactor |
| [S5.D](S5.D-reserve-and-token-xray.md) | Reserve Enforcement + Token-Cost X-Ray | Context |
| [S5.E](S5.E-pin-and-tag.md) | Per-Message Pin / Tag System | Context |
| [S5.F](S5.F-compaction-v2.md) | Compaction v2 (Toolkit + Auto + History Archive) | Context |
| [S5.G](S5.G-chat-activity-restructure.md) | Chat / Activity Restructure | UX |
| [S5.H](S5.H-edit-delete-branch.md) | Per-Message Edit / Delete + Branch & Rewind | Context |
| [S5.I](S5.I-tabs-and-sessions.md) | Multi-Tab Conversations + Session Menu Redesign | UX |
| [S5.K](S5.K-memory-bank-ui.md) | Memory Bank UI Viewer / Editor | UX |
| [S5.Z](S5.Z-misc-gaps.md) | Miscellaneous Smaller Gaps | Mixed |

More stages will be added here as polish/UX/perf candidates surface. Letters are identity, not order -- listed top-down in execution order. S5.Z is the misc bucket at the end (mirrors [S4.V](../M4/S4.V-misc-gaps.md)) and is expected to grow.

### Context-management cluster (S5.J + S5.D - S5.H, plus S5.I)

Six stages forming a coherent context-management overhaul, designed for short-context local LLMs (32k typical for coding work). Dependency order:

0. **S5.J** lands first -- pure refactor introducing the `LLMContext` class as the single owner of conversation state (history, system prompt, budget, file tracker, attached context, archives, checkpoints). Splits AgentCore back into "agent loop runner" + "LLM-view state owner." Zero user-visible changes; the safety net is the existing test suite (refactored to construct `LLMContext` directly where it makes the test simpler). **Prerequisite for everything below** -- features in S5.D - S5.I land on the LLMContext surface S5.J establishes.
1. **S5.D** -- reserve enforcement + per-message token visibility are the user-facing foundation; everything else depends on them.
2. **S5.E** opens user control over what survives compaction (3-state pin button per message).
3. **S5.F** ships the composable compaction toolkit (six layers run as a pipeline), threshold bands, auto-compaction for unattended sessions, and the history-archive backup chain.
4. **S5.G** restructures chat to be the LLM's view (system prompt visible, per-message token / pin controls integrated) and activity log to be pure system audit (gains missing indexing/embedding events).
5. **S5.H** adds the surgical operations -- edit, delete, rewind, branch, restore-from-archive.
6. **S5.I** -- multi-tab conversations. Each tab owns one `LLMContext` + one `AgentCore`, sharing the workspace and LLM client. Trivial to implement once S5.J lands; would have been a multi-week refactor without it.

Each stage is shippable independently after S5.J and the user-visible value compounds.

## Suggested order

S5.L (UI Automation Test Driver) lands first: it closes the GUI-testing gap the rest of M5 is about to widen -- every UX change in this milestone (Terminal Panel, Inline Diffs, Chat / Activity Restructure, Tabs, Memory Bank UI) adds wx widgets we currently can only verify by hand. A scripted UIA driver makes those widgets subprocess-testable from day one, before the surface area grows. S5.B (Terminal Panel) follows as the most visible UX gap. S5.A (Capability Toggles) is the manifest-pruning win for short-context local LLMs. The three are independent; reorder if priorities shift.
