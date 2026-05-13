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
| **[M0 -- CLI Prototype](roadmap/M0.md)** ✔ | Prove the hard parts work before any UI | WS1 (Locus project) |
| **[M1 -- Desktop App](roadmap/M1.md)** ✔ | Usable standalone tool with wxWidgets UI | WS1 |
| **[M2 -- Full Workspace Support](roadmap/M2.md)** ✔ | All 3 test workspaces functional | WS1, WS2 (Wikipedia), WS3 (Docs) |
| **[M3 -- Refactoring](roadmap/M3/README.md)** ✔ | Pay down architectural debt before the next big feature push (split god-classes, regularize layering, document threading model) | WS1 |
| **[M4 -- Agent Quality](roadmap/M4/README.md)** ✔ | Close the gap vs RooCode/Aider/Cline/OpenCode/Claude Code on the tech fundamentals of a coding agent (editing, verification, undo, planning, extensibility, retrieval quality) | WS1 |
| **[M5 -- Polish, UX & Performance](roadmap/M5/README.md)** | Sand down the user-visible edges before multiplying frontends -- UX refinements, performance optimization, robustness, quality-of-life touches | WS1, WS2, WS3 |
| **[M6 -- Connected](roadmap/M6/README.md)** | Remote access, VS Code shim, web frontend | All, from any device |

M0-M2 each live in a single milestone file (completed history). M3, M4, M5, M6 use one folder per milestone with a `README.md` index plus one file per stage so each stays individually greppable as it moves from planned -> scoping -> in-progress.

---

## M0 -- CLI Prototype ✔

Complete. Stage list and full task history in [roadmap/M0.md](roadmap/M0.md).

## M1 -- Desktop App ✔

Complete. Stage list and full task history in [roadmap/M1.md](roadmap/M1.md).

## M2 -- Full Workspace Support ✔

Complete. Stage list and full task history in [roadmap/M2.md](roadmap/M2.md).

## M3 -- Refactoring ✔

Complete. All 12 stages landed -- god-classes split, `src/` layered consistently, threading model documented, ADR trail in place, tool catalog consolidated. Stage list and full task history: [roadmap/M3/README.md](roadmap/M3/README.md).

## M4 -- Agent Quality ✔

Complete. Closed the gap against leading agents on the tech fundamentals -- diff-based editing, checkpoint/undo, retrieval (better embeddings + reranker + eval harness), background commands, telemetry, AST search, file-change awareness between turns, tool-call robustness across model families, stream decode completeness + max_tokens discipline, plan mode, MCP client, memory bank (Phase 1), git integration (gitignore + auto-commit), KV/prompt cache preservation, list_directory completeness, prompt templates, grammar coverage expansion, plus the S4.V miscellaneous-gaps cleanup (C symbol extractor + curated AST queries, `@`-mention file references, workspace tool-trust + outside-workspace path warnings, model-card presets, sampler controls, prompt/generation split in the context meter). Stage list and full task history: [roadmap/M4/README.md](roadmap/M4/README.md).

## M5 -- Polish, UX & Performance

With the M4 engine in place, sand down the user-visible edges. UX refinements, performance optimization, robustness work, and quality-of-life touches that make the existing capability set feel fast, predictable, and discoverable. Lands before M6 so each new frontend in the Connected milestone inherits a well-shaped foundation.

Progress: **S5.L (UI Automation Test Driver) ✔** -- ships a manual-only `locus_ui_tests` target driving `locus_gui.exe` via Windows UIA from JSON scripts; three demo scripts (smoke / settings_tour / chat_round_trip) all pass. See [roadmap/M5/S5.L-ui-automation-driver.md](roadmap/M5/S5.L-ui-automation-driver.md).

Stage list: [roadmap/M5/README.md](roadmap/M5/README.md).

## M6 -- Connected

Core accessible over LAN. VS Code sends editor context. Browser frontend works. Wikipedia works end-to-end.

Stage list and inter-stage dependencies: [roadmap/M6/README.md](roadmap/M6/README.md).

---

## Backlog

Unscheduled items + parked drafts -- recognised as worth doing eventually, not yet scoped or sequenced into a milestone. See [roadmap/backlog/README.md](roadmap/backlog/README.md).
