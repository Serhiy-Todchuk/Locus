# <Project> -- AI Working Guide

<!--
This is the CLAUDE.md-equivalent: the agent's always-loaded operational reference.
Rules for keeping it useful:
  - Laconic. Engineers, not book writers. Link, don't copy.
  - It is NOT a design doc. Narrative lives in per-stage docs; this file points at them.
  - After a stage, edits here are one-liners (add to the done list), not new paragraphs.
Delete this comment block once you've filled it in.
-->

## Project in One Sentence

<one sentence: what this is and the single defining constraint>

## Owner Profile

- **Background**: <languages, years, domain>
- **Priorities**: <perf? simplicity? correctness? what does the owner hate?>
- **Control style**: <step-by-step vs autonomous; how much visibility they want>

**How to code for this owner:**
- <e.g. don't add abstraction layers unless necessary>
- <e.g. no padding/summaries; direct efficient solutions>
- <e.g. treat as senior; skip basics>

## Current Stage

**M<N> -- <milestone name> -- in progress.** Done: <S-codes>. Detail in [roadmap/M<N>/](...).

**M<N-1> -- <name> -- complete.** Done: <S-codes>.

### Key invariants

<!-- The crown jewel. Cross-cutting rules a refactor in a NEIGHBORING file could break
silently. Each entry: what the rule is, which files enforce it, and WHY. Add one only when
a stage introduces a real tripwire. -->

- **<Invariant name>** (S<code>). <What must stay true>. <Which files>. <Why breaking it is bad>.

## Key Decisions Made

| Decision | Choice | Rationale |
|---|---|---|
| <topic> | **<choice>** | <one-line why> |

## Coding Conventions

- **Language / namespace / file layout**: <...>
- **Naming**: <case rules for files / types / members / constants>
- **Error handling**: <fatal vs recoverable policy>
- **Ownership**: <RAII, unique_ptr, copy/move policy>
- **Forbidden**: <the explicit "never do this" list>
- **Tests**: <framework, one file per subsystem, tag-by-stage>
- **Version control**: <branch policy; commit/push only when asked; commit message rules>

## Build & Test Commands

<!-- Copy-pasteable. The agent should never have to guess how to build or test. -->

```
<configure>
<build>
<run against a real workspace>
<unit tests>
```

## Code Map

<!-- A table of every source file: what it owns + key types. Lets the agent find the right
file without walking the tree. This table is load-bearing; keep rows current when a file's
responsibility changes. -->

| File | What it owns | Key types |
|---|---|---|
| `src/<file>` | <responsibility> | `<Type>` |

## Document Map

| File | When to read |
|---|---|
| roadmap.md | milestone/stage index |
| architecture/decisions/ | ADRs - the WHY behind non-trivial shifts |
| CONTRIBUTING.md | build/run/test for humans |

## Key Architecture Principles (Non-negotiable)

1. <e.g. the deterministic component the AI never mutates>
2. <e.g. pull don't push / lazy over eager>
3. <e.g. operating transparency>
4. <e.g. trust boundary = authorship>

## Development & Testing Protocol

**After every completed stage -- blocking gate:**
1. Full build.
2. Run against a real workload.
3. Exercise the stage's features.
4. Read the log; catch what the happy path hid.
5. Fix everything unexpected before the next stage.

**After every completed stage -- mandatory bookkeeping:**
1. Mark tasks `[x]` in roadmap.
2. Add the stage code to "Current Stage" here. Add a Key Invariant only if the stage
   introduced a tripwire. Update Code Map rows if a file's job changed.
3. Write an ADR only if the change crossed a system boundary AND rejected a reasonable
   alternative for a non-obvious reason AND that reasoning isn't captured elsewhere.
