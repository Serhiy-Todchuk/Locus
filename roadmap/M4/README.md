# M4 — Agent Quality

**Goal**: Close the gap against leading agents (RooCode, Cline, Aider, OpenCode, Claude Code) on the tech fundamentals that make a coding agent actually usable day-to-day: precise edits, verification loops, undo, planning, extensibility, and retrieval quality. Not UX polish — the engine underneath.

**Test workspaces unlocked**: WS1.

**Note**: Stage letters (S4.A, S4.B, …) are identity, not order — they were assigned when the stages were drafted and are retained for link stability. The table below is listed in execution order.

## Stages (in execution order)

| Stage | Title | Theme |
|---|---|---|
| [S4.A](S4.A-diff-editing.md) | Diff-Based Editing | Editing |
| [S4.B](S4.B-checkpoint-undo.md) | Checkpoint & Undo | Safety |
| [S4.P](S4.P-grep.md) | Grep Tool (Regex over Raw Content) | Tools |
| [S4.J](S4.J-embeddings-reranker.md) | Better Embeddings + Reranker | Retrieval |
| [S4.K](S4.K-retrieval-eval.md) | Retrieval Evaluation Harness | Retrieval |
| [S4.I](S4.I-background-commands.md) | Background / Long-Running Commands | Process model |
| [S4.S](S4.S-telemetry.md) | Telemetry & Agent Performance Metrics | Observability |
| [S4.L](S4.L-git-native.md) | Git-Native Features | Tools |
| [S4.M](S4.M-ast-search.md) | Tree-Sitter Query Tool (Structural Grep) | Tools |
| [S4.T](S4.T-file-change-awareness.md) | File-Change Awareness Between Turns | Context |
| [S4.U](S4.U-subagents.md) | Subagent / Task Delegation | Workflow |
| [S4.Q](S4.Q-multi-model.md) | Multi-Model Split (Weak + Strong) | Performance |
| [S4.F](S4.F-kv-cache.md) | KV / Prompt Cache Preservation | Performance |
| [S4.N](S4.N-tool-call-robustness.md) | Tool-Call Robustness Across Model Families | LLM compat |
| [S4.E](S4.E-lsp.md) | LSP Integration | Semantic depth |
| [S4.R](S4.R-memory-bank.md) | Memory Bank / Session Scratchpad | Context |
| [S4.G](S4.G-mcp.md) | MCP (Model Context Protocol) Client | Extensibility |
| [S4.C](S4.C-auto-verify.md) | Auto-Verify Feedback Loop | Verification |
| [S4.D](S4.D-plan-mode.md) | Plan Mode | Workflow |
| [S4.H](S4.H-parallel-tools.md) | Parallel Tool Calls | Performance |
| [S4.O](S4.O-streaming-tool-results.md) | Streaming / Partial Tool Results | UX latency |
| [S4.V](S4.V-misc-gaps.md) | Miscellaneous Smaller Gaps | Mixed |

## Prerequisites from M3

- [S3.L — tool-catalog-hygiene](../M3/S3.L-tool-catalog-hygiene.md) before S4.A — every subsequent M4 stage that adds tools should be authored against the new `available()` / `visible_in_mode()` hooks instead of retrofitted.
- [S3.B — llm-client-split](../M3/S3.B-llm-client-split.md) before S4.N, S4.Q.
- [S3.G — locus-session](../M3/S3.G-locus-session.md) before S4.B — the first new lifecycle subsystem (the checkpoint store) is the natural moment to bundle wiring into `LocusSession`.

S3.A (agent-core-split) and S3.I (threading-model), also prerequisites for several M4 stages, already done.
