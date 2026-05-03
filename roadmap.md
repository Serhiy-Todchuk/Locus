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
| **[M4 -- Agent Quality](roadmap/M4/README.md)** | Close the gap vs RooCode/Aider/Cline/OpenCode/Claude Code on the tech fundamentals of a coding agent (editing, verification, undo, planning, extensibility, retrieval quality) | WS1 |
| **[M5 -- Connected](roadmap/M5/README.md)** | Remote access, VS Code shim, web frontend | All, from any device |

M0-M2 each live in a single milestone file (completed history). M3, M4, M5 use one folder per milestone with a `README.md` index plus one file per stage so each stays individually greppable as it moves from planned -> scoping -> in-progress.

---

## M0 -- CLI Prototype ✔

Complete. Stage list and full task history in [roadmap/M0.md](roadmap/M0.md).

## M1 -- Desktop App ✔

Complete. Stage list and full task history in [roadmap/M1.md](roadmap/M1.md).

## M2 -- Full Workspace Support ✔

Complete. Stage list and full task history in [roadmap/M2.md](roadmap/M2.md).

## M3 -- Refactoring ✔

Complete. All 12 stages landed -- god-classes split, `src/` layered consistently, threading model documented, ADR trail in place, tool catalog consolidated. Stage list and full task history: [roadmap/M3/README.md](roadmap/M3/README.md).

## M4 -- Agent Quality

Close the gap against leading agents (RooCode, Cline, Aider, OpenCode, Claude Code) on the tech fundamentals that make a coding agent actually usable day-to-day: precise edits, verification loops, undo, planning, extensibility, and retrieval quality. Not UX polish -- the engine underneath.

Stage list, execution order, and M3 prerequisites: [roadmap/M4/README.md](roadmap/M4/README.md).

## M5 -- Connected

Core accessible over LAN. VS Code sends editor context. Browser frontend works. Wikipedia works end-to-end.

Stage list and inter-stage dependencies: [roadmap/M5/README.md](roadmap/M5/README.md).

---

## Backlog

Unscheduled items -- recognised as worth doing eventually, not yet scoped or sequenced into a milestone. See [backlog/README.md](backlog/README.md).
