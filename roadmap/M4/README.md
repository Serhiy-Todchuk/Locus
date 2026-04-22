# M4 — Agent Quality

**Goal**: Close the gap against leading agents (RooCode, Cline, Aider, OpenCode, Claude Code) on the tech fundamentals that make a coding agent actually usable day-to-day: precise edits, verification loops, undo, planning, extensibility, and retrieval quality. Not UX polish — the engine underneath.

**Test workspaces unlocked**: WS1.

**Note**: Stages below are NOT ordered by priority or dependency yet. Lettered (S4.A, S4.B, …) instead of numbered until an ordering pass is done.

## Stages

| Stage | Title | Theme |
|---|---|---|
| [S4.A](S4.A-diff-editing.md) | Diff-Based Editing | Editing |
| [S4.B](S4.B-checkpoint-undo.md) | Checkpoint & Undo | Safety |
| [S4.C](S4.C-auto-verify.md) | Auto-Verify Feedback Loop | Verification |
| [S4.D](S4.D-plan-mode.md) | Plan Mode | Workflow |
| [S4.E](S4.E-lsp.md) | LSP Integration | Semantic depth |
| [S4.F](S4.F-kv-cache.md) | KV / Prompt Cache Preservation | Performance |
| [S4.G](S4.G-mcp.md) | MCP (Model Context Protocol) Client | Extensibility |
| [S4.H](S4.H-parallel-tools.md) | Parallel Tool Calls | Performance |
| [S4.I](S4.I-background-commands.md) | Background / Long-Running Commands | Process model |
| [S4.J](S4.J-embeddings-reranker.md) | Better Embeddings + Reranker | Retrieval |
| [S4.K](S4.K-retrieval-eval.md) | Retrieval Evaluation Harness | Retrieval |
| [S4.L](S4.L-git-native.md) | Git-Native Features | Tools |
| [S4.M](S4.M-ast-search.md) | Tree-Sitter Query Tool (Structural Grep) | Tools |
| [S4.N](S4.N-tool-call-robustness.md) | Tool-Call Robustness Across Model Families | LLM compat |
| [S4.O](S4.O-streaming-tool-results.md) | Streaming / Partial Tool Results | UX latency |
| [S4.P](S4.P-grep.md) | Grep Tool (Regex over Raw Content) | Tools |
| [S4.Q](S4.Q-multi-model.md) | Multi-Model Split (Weak + Strong) | Performance |
| [S4.R](S4.R-memory-bank.md) | Memory Bank / Session Scratchpad | Context |
| [S4.S](S4.S-telemetry.md) | Telemetry & Agent Performance Metrics | Observability |
| [S4.T](S4.T-file-change-awareness.md) | File-Change Awareness Between Turns | Context |
| [S4.U](S4.U-subagents.md) | Subagent / Task Delegation | Workflow |
| [S4.V](S4.V-misc-gaps.md) | Miscellaneous Smaller Gaps | Mixed |

## Suggested ordering

Two prerequisites from M3 should land first to keep this milestone sane:
- [S3.A — agent-core-split](../M3/S3.A-agent-core-split.md) before S4.D, S4.H, S4.U.
- [S3.B — llm-client-split](../M3/S3.B-llm-client-split.md) before S4.N, S4.Q.
- [S3.I — threading-model](../M3/S3.I-threading-model.md) before S4.E, S4.G, S4.H, S4.I, S4.U.

Within M4, sensible early stages (low blast radius, high daily value):
**S4.A** (diff editing), **S4.B** (undo), **S4.C** (verify) — these three together transform daily usability.
