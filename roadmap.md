# Locus — Roadmap

Three levels: **Milestone → Stage → Task**

Milestones are major shippable states. Stages are coherent units of work within a milestone.
Tasks are concrete implementation items — specific enough to start coding without further design.

## Testing Protocol

**Test workspace**: `d:\Projects\AICodeAss\` (WS1 — the Locus project itself).
Locus always runs against its own folder during development. What is indexed is what is visible.

**After every stage is complete:**
1. Build
2. Run: `locus.exe d:\Projects\AICodeAss -verbose`
3. Exercise the stage's features
4. Quit → read `.locus\locus.log`
5. Fix anything unexpected before moving to the next stage

`-verbose` drops spdlog to `trace` level: SQL queries, tool args/results, LLM stream, index events.
Default (`info`) logs only errors, warnings, and stage transitions.

---

## Overview

| Milestone | Goal | Test workspaces unlocked |
|---|---|---|
| **M0 — CLI Prototype** | Prove the hard parts work before any UI | WS1 (Locus project) |
| **M1 — Desktop App** | Usable standalone tool with wxWidgets UI | WS1 |
| **M2 — Full Workspace Support** | All 3 test workspaces functional | WS1, WS2 (Wikipedia), WS3 (Docs) |
| **M3 — Refactoring** | Pay down architectural debt before the next big feature push (split god-classes, regularize layering, document threading model) | WS1 |
| **M4 — Agent Quality** | Close the gap vs RooCode/Aider/Cline/OpenCode/Claude Code on the tech fundamentals of a coding agent (editing, verification, undo, planning, extensibility, retrieval quality) | WS1 |
| **M5 — Connected** | Remote access, VS Code shim, web frontend | All, from any device |

Per-milestone detail lives in `roadmap/`. M0–M2 each live in a single milestone file (completed history). M3, M4, M5 use one file per stage so each stays individually greppable as it moves from planned → scoping → in-progress.

---

## M0 — CLI Prototype ✔

Complete. Full task list: [roadmap/M0.md](roadmap/M0.md).

| Stage | Title |
|---|---|
| S0.1 | Project Setup ✔ |
| S0.2 | Workspace Foundation ✔ |
| S0.3 | FTS5 Index ✔ |
| S0.4 | Index Query API ✔ |
| S0.5 | LLM Client ✔ |
| S0.6 | Tool System ✔ |
| S0.7 | Agent Core ✔ |
| S0.8 | CLI Frontend ✔ |
| S0.9 | M0 Validation ✔ |

---

## M1 — Desktop App ✔

Complete. Full task list: [roadmap/M1.md](roadmap/M1.md).

| Stage | Title |
|---|---|
| S1.1 | Core / Frontend Architecture ✔ |
| S1.2 | wxWidgets Bootstrap + System Tray ✔ |
| S1.3 | Chat UI + Markdown ✔ |
| S1.4 | Tool Approval UI ✔ |
| S1.5 | Context Compaction UI ✔ |
| S1.6 | Workspace, File Tree & Settings ✔ |

---

## M2 — Full Workspace Support ✔

Complete. Full task list: [roadmap/M2.md](roadmap/M2.md).

| Stage | Title |
|---|---|
| S2.1 | Semantic Search ✔ |
| S2.2 | Activity Details Panel ✔ |
| S2.3 | Document Text Extraction ✔ |
| S2.4 | Attach File to Context ✔ |

---

## M3 — Refactoring

**Goal**: Pay down architectural debt before the next big feature push (M4 Agent Quality, M5 Connected). Split the god-classes that are about to collide with every new feature, regularize folder layout, document threading and the agent loop. Low-risk, high-future-value.

Per-stage detail: [roadmap/M3/](roadmap/M3/) — see [README](roadmap/M3/README.md) for the suggested ordering.

| Stage | Title |
|---|---|
| [S3.A](roadmap/M3/S3.A-agent-core-split.md) | Split `AgentCore` (850 LOC) into agent_loop / tool_dispatcher / activity_log / context_budget / session_store collaborators |
| [S3.B](roadmap/M3/S3.B-llm-client-split.md) | Split `ILLMClient` / `do_stream` into transport + stream decoder + token counter; multi-format + multi-endpoint ready |
| [S3.C](roadmap/M3/S3.C-workspace-services.md) | Replace `WorkspaceContext` raw-pointer struct with `IWorkspaceServices` interface |
| [S3.D](roadmap/M3/S3.D-indexer-split.md) | Apply extractor-registry pattern to Tree-sitter; extract symbol rules + grammars + prepared-statements holder |
| [S3.E](roadmap/M3/S3.E-tools-folder.md) ✔ | Split `tools.cpp` (21 KB / 12 tools) into `src/tools/` subfolder by family |
| [S3.F](roadmap/M3/S3.F-locus-frame-split.md) | Slim `LocusFrame` (900 LOC); move file-watcher pump from GUI into `Workspace` (deduplicate 3 copies) |
| [S3.G](roadmap/M3/S3.G-locus-session.md) | Bundle Workspace + LLM + ToolRegistry + AgentCore into `LocusSession` with single ctor/dtor |
| [S3.H](roadmap/M3/S3.H-src-layering.md) | Consistent `src/{core,agent,llm,index,tools,extractors,frontends,util}/` layout |
| [S3.I](roadmap/M3/S3.I-threading-model.md) ✔ | Document threads + ownership rules + cross-thread invariants |
| [S3.J](roadmap/M3/S3.J-slash-commands.md) | Extract slash-command tokenizer + dispatcher from `AgentCore` into its own module |
| [S3.K](roadmap/M3/S3.K-docs.md) ✔ | Add `agent-loop.md`, `threading-model.md`; introduce ADR trail under `architecture/decisions/` |
| [S3.L](roadmap/M3/S3.L-tool-catalog-hygiene.md) ✔ | Tool-catalog hygiene: consolidate search family into `search(mode=…)`; add `available()` / `visible_in_mode()` gating hooks; log per-turn manifest size with threshold warning |

---

## M4 — Agent Quality

**Goal**: Close the gap against leading agents (RooCode, Cline, Aider, OpenCode, Claude Code) on the tech fundamentals that make a coding agent actually usable day-to-day: precise edits, verification loops, undo, planning, extensibility, and retrieval quality. Not UX polish — the engine underneath.

Per-stage detail: [roadmap/M4/](roadmap/M4/) — see [README](roadmap/M4/README.md) for ordering and M3 prerequisites.

Listed in execution order. Letters are identity, not sequence (retained for link stability).

| Stage | Title |
|---|---|
| [S4.A](roadmap/M4/S4.A-diff-editing.md) | Diff-Based Editing ✔ |
| [S4.B](roadmap/M4/S4.B-checkpoint-undo.md) | Checkpoint & Undo |
| [S4.P](roadmap/M4/S4.P-grep.md) | Grep Tool (Regex over Raw Content) |
| [S4.J](roadmap/M4/S4.J-embeddings-reranker.md) | Better Embeddings + Reranker |
| [S4.K](roadmap/M4/S4.K-retrieval-eval.md) | Retrieval Evaluation Harness |
| [S4.I](roadmap/M4/S4.I-background-commands.md) | Background / Long-Running Commands |
| [S4.S](roadmap/M4/S4.S-telemetry.md) | Telemetry & Agent Performance Metrics |
| [S4.L](roadmap/M4/S4.L-git-native.md) | Git-Native Features |
| [S4.M](roadmap/M4/S4.M-ast-search.md) | Tree-Sitter Query Tool (Structural Grep) |
| [S4.T](roadmap/M4/S4.T-file-change-awareness.md) | File-Change Awareness Between Turns |
| [S4.U](roadmap/M4/S4.U-subagents.md) | Subagent / Task Delegation |
| [S4.Q](roadmap/M4/S4.Q-multi-model.md) | Multi-Model Split (Weak + Strong) |
| [S4.F](roadmap/M4/S4.F-kv-cache.md) | KV / Prompt Cache Preservation |
| [S4.N](roadmap/M4/S4.N-tool-call-robustness.md) | Tool-Call Robustness Across Model Families |
| [S4.E](roadmap/M4/S4.E-lsp.md) | LSP Integration |
| [S4.R](roadmap/M4/S4.R-memory-bank.md) | Memory Bank / Session Scratchpad |
| [S4.G](roadmap/M4/S4.G-mcp.md) | MCP (Model Context Protocol) Client |
| [S4.C](roadmap/M4/S4.C-auto-verify.md) | Auto-Verify Feedback Loop |
| [S4.D](roadmap/M4/S4.D-plan-mode.md) | Plan Mode |
| [S4.H](roadmap/M4/S4.H-parallel-tools.md) | Parallel Tool Calls |
| [S4.O](roadmap/M4/S4.O-streaming-tool-results.md) | Streaming / Partial Tool Results |
| [S4.V](roadmap/M4/S4.V-misc-gaps.md) | Miscellaneous Smaller Gaps |

---

## M5 — Connected

**Goal**: Core accessible over LAN. VS Code sends editor context. Browser frontend works. Wikipedia works end-to-end.

Per-stage detail: [roadmap/M5/](roadmap/M5/) — see [README](roadmap/M5/README.md).

| Stage | Title |
|---|---|
| [S5.1](roadmap/M5/S5.1-web-retrieval.md) | Web Retrieval (RAG) |
| [S5.2](roadmap/M5/S5.2-zim-reader.md) | ZIM Reader (Wikipedia / Kiwix) |
| [S5.3](roadmap/M5/S5.3-crow-frontend.md) | CrowServer Frontend |
| [S5.4](roadmap/M5/S5.4-remote-access.md) | Remote Access |
| [S5.5](roadmap/M5/S5.5-web-frontend.md) | Web / Browser Frontend |
| [S5.6](roadmap/M5/S5.6-vscode-shim.md) | VS Code Shim |

---

## Deferred (Post-M5)

Items from requirements Nice-to-Have — not scheduled yet:

- Native mobile app (CrowServer client)
- Voice chat
- Feeding an image as an inut to LLM
- Online model backends (Claude, OpenAI, Gemini)
- Full VS Code extension (embedded UI panel, not just shim)
- Plugin system for community tools
- Approve or modify tool actions
- Keep working when context fills up
- Line-level diff viewer for AI-proposed file changes (wxStyledTextCtrl, dtl library)
- Multiple workspaces open simultaneously (tabbed sessions)
- Export session as markdown report
- Automated coding pipeline (plan → code → test → fix loop, user-supervised)
- LaTeX math display in LLM chat (for formulas)
- Symbol index: extract member variables / field declarations (C++ `field_declaration`, etc.)
- Bundled `LlamaCppClient` as a second `ILLMClient` backend (optional). Reuses llama.cpp already linked for embeddings; gives zero-install first-run with a curated GGUF. Keeps LM Studio / Ollama / llama-server path as primary (any OpenAI-compatible endpoint). Costs: GPU-backend distribution (CPU + Vulkan realistic, CUDA adds hundreds of MB), per-model chat-template + tool-call parsing we currently get free from LM Studio, and model-management UX (picker, download, swap).
- Open-citation UX: agent cites `file:line` or `file:page` → click opens the right thing per format. Shell-execute for PDF/DOCX/XLSX (native app), in-app render for ZIM articles + markdown + plain text (reuse existing WebView/md4c), hand off to VS Code for code files. Avoids building a universal file viewer while still letting the user verify what the agent saw. ZIM article rendering is the only hard requirement (no external viewer exists) and is implicitly needed by [S5.2](roadmap/M5/S5.2-zim-reader.md).
- Lightweight chat UI: replace `wxWebView` with `wxHtmlWindow` (optional). Keeps md4c → HTML pipeline, drops the browser engine (tens of MB → hundreds of KB per instance, no cold-start cost). Loses: JS-powered features (Mermaid, KaTeX, interactive code views) — those would need WebView back for just those views.
- CLI: raw-mode line editor (vendor replxx or similar) — suppresses visible `^[[200~` bracketed-paste markers on Windows, adds history, arrow-key editing. Current CLI uses line-mode stdin + terminal echo, which renders VT input sequences literally during paste. Non-blocking since desktop GUI is the primary frontend.
