# M5 -- Polish, UX & Performance -- COMPLETE (2026-05-19)

All 18 stages closed. S5.Z remains the parking lot if polish-shaped work
surfaces post-close; reopening adds tasks to its list without re-opening the
milestone as a whole.

**Goal**: With the M4 engine in place, sand down the user-visible edges. UX refinements, performance optimization, robustness work, and quality-of-life touches that make the existing capability set feel fast, predictable, and discoverable. No big new subsystems -- this milestone is about the experience of using what we've built.

**Test workspaces unlocked**: WS1, WS2, WS3 (no new workspace requirements).

**Why before M6 (Connected & Misc)**: M6 multiplies surface area (remote frontends, web UI, VS Code shim, ZIM reader, plus assorted non-polish work). Polishing the local experience first means each new frontend in M6 inherits a well-shaped foundation rather than papering over rough edges in three places.

## Stages (in execution order)

- [x] **[S5.L](S5.L-ui-automation-driver.md)** -- UI Automation Test Driver -- _Testing / Infra_
- [x] **[S5.B](S5.B-terminal-panel.md)** -- Live Terminal Panel -- _UX_
- [x] **[S5.A](S5.A-capability-toggles.md)** -- Workspace Capability Toggles -- _UX / Tokens_
- [x] **[S5.M](S5.M-global-settings.md)** -- Global Settings Template + ~/.locus migration -- _UX / Config_
- [x] **[S5.C](S5.C-inline-diffs-in-chat.md)** -- Inline Code Diffs in Chat -- _UX_
- [x] **[S5.J](S5.J-llm-context-refactor.md)** -- LLMContext Class Refactor -- _Refactor_
- [x] **[S5.O](S5.O-tool-safety-hardening.md)** -- Tool & Filesystem Safety Hardening -- _Safety_
- [x] **[S5.P](S5.P-source-encoding-audit.md)** -- Source Encoding Audit & Lint -- _Hygiene_
- [x] **[S5.Q](S5.Q-large-file-decomposition.md)** -- Large File Decomposition (chat_panel & settings_dialog) -- _Refactor_
- [x] **[S5.D](S5.D-reserve-and-token-xray.md)** -- Reserve Enforcement + Token-Cost X-Ray -- _Context_
- [x] **[S5.F](S5.F-compaction-v2.md)** -- Compaction v2 (Toolkit + Auto + History Archive) -- _Context_
- [x] **[S5.G](S5.G-chat-activity-restructure.md)** -- Chat / Activity Restructure -- _UX_
- [x] **[S5.I](S5.I-tabs-and-sessions.md)** -- Multi-Tab Conversations + Session Menu Redesign -- _UX_
- [x] **[S5.R](S5.R-per-tab-terminal.md)** -- Per-Tab Terminal Panel -- _UX_
- [x] **[S5.K](S5.K-memory-bank-ui.md)** -- Memory Bank UI Viewer / Editor -- _UX_
- [x] **[S5.N](S5.N-non-code-workspace-proof.md)** -- Non-Code Workspace Proof -- _Product / QA_
- [x] **[S5.S](S5.S-permission-presets.md)** -- Permission Presets -- _Safety / UX_
- [x] **[S5.Z](S5.Z-misc-gaps.md)** -- Miscellaneous Smaller Gaps -- _Mixed_

More stages will be added here as polish/UX/perf candidates surface. Letters are identity, not order -- listed top-down in execution order. S5.Z is the misc bucket at the end (mirrors [S4.V](../M4/S4.V-misc-gaps.md)) and is expected to grow.

### Robustness cluster (S5.O - S5.Q)

Three stages added 2026-05-15 in response to an external code-review pass. Independent of the context-management cluster; can land in parallel.

- **S5.O** closes three latent safety holes in the tool layer (path containment, approval call-id keying, atomic `write_file`). Highest priority -- the bugs are real and the workspace boundary is what every file tool relies on.
- **S5.P** sweeps em-dashes and other non-ASCII drift from source + adds a build-time lint to keep them out. The "no em-dashes" rule is in CLAUDE.md but is currently enforced by trust alone.
- **S5.Q** breaks `chat_panel.cpp` (81 KB) and `settings_dialog.cpp` (61 KB) into cohesive sub-units so future polish work has a smaller blast radius. `agent_core.cpp` is intentionally excluded -- S5.J already pulled `LLMContext` out, and the rest is the indivisible agent-loop runner.

### Context-management cluster (S5.J + S5.D + S5.F + S5.G + S5.I)

Five stages forming a coherent context-management overhaul, designed for short-context local LLMs (32k typical for coding work). Dependency order:

0. **S5.J** lands first -- pure refactor introducing the `LLMContext` class as the single owner of conversation state (history, system prompt, budget, file tracker, attached context, archives, checkpoints). Splits AgentCore back into "agent loop runner" + "LLM-view state owner." Zero user-visible changes; the safety net is the existing test suite (refactored to construct `LLMContext` directly where it makes the test simpler). **Prerequisite for everything below** -- features in S5.D + S5.F + S5.G + S5.I land on the LLMContext surface S5.J establishes.
1. **S5.D** -- reserve enforcement + per-message token visibility are the user-facing foundation; everything else depends on them.
2. **S5.F** ships the composable compaction toolkit (six layers run as a pipeline), threshold bands, auto-compaction for unattended sessions, and the history-archive backup chain.
3. **S5.G** restructures chat to be the LLM's view (system prompt visible, per-message token controls, per-message hover-reveal delete button) and activity log to be pure system audit (gains missing indexing/embedding events).
4. **S5.I** -- multi-tab conversations. Each tab owns one `LLMContext` + one `AgentCore`, sharing the workspace and LLM client. Trivial to implement once S5.J lands; would have been a multi-week refactor without it.

Each stage is shippable independently after S5.J and the user-visible value compounds.

Two stages from the original cluster were demoted to the backlog on 2026-05-16 after a value-vs-effort review:

- **[S5.E (Per-Message Pin / Tag System)](../backlog/S5.E-pin-and-tag.md)**. Five of S5.F's six compaction layers don't need pin state; the "preserve critical messages" use case has cheaper covers (system prompt already byte-stable, recent N turns + attached-context auto-preserved by heuristic, inline pin/unpin in S5.F's CompactionView preview); persisting a 3-state field on every `ChatMessage` was committing the saved-session JSON schema to a policy that hadn't been validated against real workloads. Land S5.F with implicit heuristics first; reactivate S5.E if real usage surfaces the need.
- **[S5.H (Per-Message Edit / Branch & Rewind)](../backlog/S5.H-edit-delete-branch.md)**. The load-bearing slice -- per-message delete -- moved into [S5.G](S5.G-chat-activity-restructure.md) as a hover-reveal X button (no archive infrastructure; "delete the last few messages and retype" covers the rewind workflow). Edit is a footgun (rewrites history the next turn's KV cache already assumed otherwise); rewind / branch / restore-from-archive / "Move to new tab from here" are speculative infrastructure for a workflow nobody has yet. Reactivate if real usage shows users wanting to edit (not delete-and-retype) past messages or a parallel-exploration workflow surfaces that delete+multi-tab doesn't cover.

### Non-code proof stage (S5.N)

Locus's positioning depends on serving document folders and offline knowledge bases,
not only code repositories. S5.N turns that claim into a repeatable WS2/WS3 demo and
QA artifact before M6 adds more frontends. It should land after the context-management
cluster is stable enough for long document sessions, but before web/remote surfaces
make the non-code story more visible.

## Suggested order

S5.L (UI Automation Test Driver) lands first: it closes the GUI-testing gap the rest of M5 is about to widen -- every UX change in this milestone (Terminal Panel, Inline Diffs, Chat / Activity Restructure, Tabs, Memory Bank UI) adds wx widgets we currently can only verify by hand. A scripted UIA driver makes those widgets subprocess-testable from day one, before the surface area grows. S5.B (Terminal Panel) follows as the most visible UX gap. S5.A (Capability Toggles) is the manifest-pruning win for short-context local LLMs. The three are independent; reorder if priorities shift.
