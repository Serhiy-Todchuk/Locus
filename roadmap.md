# Locus -- Roadmap

Three levels: **Milestone -> Stage -> Task**

Milestones are major shippable states. Stages are coherent units of work within a milestone. Tasks are concrete implementation items -- specific enough to start coding without further design.

This file is the milestone index. Stage detail lives in each milestone's own document; this file deliberately does not duplicate stage tables -- update one place, not two.

## Testing Protocol

**Test workspace**: `d:\Projects\AICodeAss\` (WS1 -- the Locus project itself). Locus always runs against its own folder during development. What is indexed is what is visible.

**After every stage is complete:**

1. Build
2. Run: `locus.exe d:\Projects\AICodeAss -verbose`
3. Exercise the stage's features
4. Quit -> read `.locus\locus.log`
5. Fix anything unexpected before moving to the next stage

`-verbose` drops spdlog to `trace` level: SQL queries, tool args/results, LLM stream, index events. Default (`info`) logs only errors, warnings, and stage transitions.

---

## Overview

| Milestone | Goal | Test workspaces unlocked |
|---|---|---|
| **[M0 -- CLI Prototype](roadmap/M0.md)** done | Prove the hard parts work before any UI | WS1 (Locus project) |
| **[M1 -- Desktop App](roadmap/M1.md)** done | Usable standalone tool with wxWidgets UI | WS1 |
| **[M2 -- Full Workspace Support](roadmap/M2.md)** done | All 3 test workspaces functional | WS1, WS2 (Wikipedia), WS3 (Docs) |
| **[M3 -- Refactoring](roadmap/M3/README.md)** done | Pay down architectural debt before the next big feature push (split god-classes, regularize layering, document threading model) | WS1 |
| **[M4 -- Agent Quality](roadmap/M4/README.md)** done | Close the gap vs RooCode/Aider/Cline/OpenCode/Claude Code on the tech fundamentals of a coding agent (editing, verification, undo, planning, extensibility, retrieval quality) | WS1 |
| **[M5 -- Polish, UX & Performance](roadmap/M5/README.md)** | Sand down the user-visible edges before multiplying frontends -- UX refinements, performance optimization, robustness, quality-of-life touches | WS1, WS2, WS3 |
| **[M6 -- Connected](roadmap/M6/README.md)** | Remote access, VS Code shim, web frontend | All, from any device |

M0-M2 each live in a single milestone file (completed history). M3, M4, M5, M6 use one folder per milestone with a `README.md` index plus one file per stage so each stays individually greppable as it moves from planned -> scoping -> in-progress.

---

## M0 -- CLI Prototype done

Complete. Stage list and full task history in [roadmap/M0.md](roadmap/M0.md).

## M1 -- Desktop App done

Complete. Stage list and full task history in [roadmap/M1.md](roadmap/M1.md).

## M2 -- Full Workspace Support done

Complete. Stage list and full task history in [roadmap/M2.md](roadmap/M2.md).

## M3 -- Refactoring done

Complete. All 12 stages landed -- god-classes split, `src/` layered consistently, threading model documented, ADR trail in place, tool catalog consolidated. Stage list and full task history: [roadmap/M3/README.md](roadmap/M3/README.md).

## M4 -- Agent Quality done

Complete. Closed the gap against leading agents on the tech fundamentals -- diff-based editing, checkpoint/undo, retrieval (better embeddings + reranker + eval harness), background commands, telemetry, AST search, file-change awareness between turns, tool-call robustness across model families, stream decode completeness + max_tokens discipline, plan mode, MCP client, memory bank (Phase 1), git integration (gitignore + auto-commit), KV/prompt cache preservation, list_directory completeness, prompt templates, grammar coverage expansion, plus the S4.V miscellaneous-gaps cleanup (C symbol extractor + curated AST queries, `@`-mention file references, workspace tool-trust + outside-workspace path warnings, model-card presets, sampler controls, prompt/generation split in the context meter). Stage list and full task history: [roadmap/M4/README.md](roadmap/M4/README.md).

## M5 -- Polish, UX & Performance

With the M4 engine in place, sand down the user-visible edges. UX refinements, performance optimization, robustness work, and quality-of-life touches that make the existing capability set feel fast, predictable, and discoverable. Lands before M6 so each new frontend in the Connected milestone inherits a well-shaped foundation.

Implementation baseline: the engine already includes tool approvals, checkpoints and `/undo`, plan mode, MCP, multi-decoder stream parsing, background commands, metrics, semantic/hybrid retrieval with reranking, memory bank, git-aware workflows, prompt templates, and UI automation hooks. M5 is therefore not about proving the agent exists; it is about making those shipped capabilities feel coherent, debuggable, and safe enough to carry into more frontends.

Progress: **S5.L (UI Automation Test Driver) done** -- ships a manual-only `locus_ui_tests` target driving `locus_gui.exe` via Windows UIA from JSON scripts; three demo scripts (smoke / settings_tour / chat_round_trip) all pass. See [roadmap/M5/S5.L-ui-automation-driver.md](roadmap/M5/S5.L-ui-automation-driver.md). **S5.B / S5.A / S5.M / S5.C done.** **S5.J (LLMContext refactor) done** -- the conversation's full LLM-facing state (history + system prompt + budget + file tracker + attached context + checkpoints + session ids) now lives on a single `LLMContext` class; `AgentCore` composes one as `ctx_` and is back to the agent-loop runner role. System prompt assembly moved into a new immutable `SystemPromptAssembly` type, turning the S4.F byte-stable invariant from discipline into a type-level guarantee. **S5.O (Tool & Filesystem Safety Hardening) done** -- closed three latent safety holes flagged by external review: path-containment now walks components (sibling `<workspace>-evil` no longer slips through the byte-prefix check; Windows case-insensitive); approval decisions are keyed by `call_id` so a stale or late click can't wake the wrong dispatch (stale decisions are dropped + warn-logged); `write_file` writes through the same atomic temp+rename helper `edit_file` uses, so a kill mid-write leaves either the previous bytes or the new bytes -- never a half-written file. **S5.P (Source Encoding Audit & Lint) done** -- swept 94 source files clean of em-dashes and other non-ASCII drift (comments, log strings, help text); the plan-step status glyphs in `chat_panel.cpp` (U+2713/U+2717/U+29D7/U+25CB) are preserved on an explicit allow-list. New `POST_BUILD` step on `locus_core` delegates to `scripts/check_no_em_dashes.py` and fails the build if a violation slips in; `scripts/strip_em_dashes.ps1` is the re-run tool. Two `[s5.p]` Catch2 cases in `tests/test_source_hygiene.cpp` verify every allow-list entry references a real file. **S5.Q (Large File Decomposition) done** -- pure structural refactor, zero behavior change. `chat_panel.cpp` split into `ChatPanel` coordinator + four collaborators under `src/frontends/gui/chat/` (`ChatStreamRenderer` / `ChatFooterChips` / `ChatPopups` / `ChatLinkHandler`). `settings_dialog.cpp` split into a thin shell + five per-tab panel classes under `src/frontends/gui/settings/` (`LlmSettingsPanel` / `IndexSettingsPanel` / `CapabilitiesSettingsPanel` / `ToolApprovalsSettingsPanel` / `McpSettingsPanel`) each implementing a common `ISettingsPanel` interface. All 488 unit tests pass; `ui_names.h` automation IDs unchanged. Tabs (S5.I), composable compaction (S5.F), chat restructure + per-message delete (S5.G), and reserve enforcement (S5.D) all land on the surface S5.J establishes (persistent pin states from the original S5.E and edit/rewind/branch from the original S5.H both demoted to backlog 2026-05-16; per-message delete -- the load-bearing slice of S5.H -- absorbed into S5.G). **S5.N (Non-Code Workspace Proof)** is now scheduled to turn the WS2/WS3 document/knowledge-base promise into a repeatable demo and QA artifact before M6 multiplies frontends.

Stage list: [roadmap/M5/README.md](roadmap/M5/README.md).

## M6 -- Connected

Core accessible over LAN. VS Code sends editor context. Browser frontend works. Wikipedia works end-to-end.

Stage list and inter-stage dependencies: [roadmap/M6/README.md](roadmap/M6/README.md).

---

## Backlog

Unscheduled items + parked drafts -- recognised as worth doing eventually, not yet scoped or sequenced into a milestone. See [roadmap/backlog/README.md](roadmap/backlog/README.md).
