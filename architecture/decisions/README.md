# Architecture Decision Records

Short, dated records explaining **why** non-trivial architectural changes were made.

The [Key Decisions](../../CLAUDE.md#key-decisions-made) table in CLAUDE.md is a *snapshot* of the
current choices. ADRs are the *history* — they preserve the reasoning behind pivots so nobody
has to re-derive it six months later.

## When to write one

The bar is **high**. Only when all three hold:

1. Crosses a system boundary — technology swap, subsystem reshape, ownership/threading/data-format change. Not interface tidy-ups or file moves.
2. Rejects a reasonable alternative for a non-obvious reason (past incident, subtle constraint, measured tradeoff). "Cleaner code" doesn't count.
3. The reasoning isn't already in the stage doc, `tech-stack.md`, CLAUDE.md, or an inline comment.

Cleared the bar: ONNX → llama.cpp (0002), ImGui → wxWidgets (0003), split index DB (0001). When in doubt, don't — ask first.

## Format

Filename: `NNNN-kebab-case-title.md`, four-digit sequence starting at `0001`.

Keep each ADR short — one page. Structure:

```markdown
# NNNN. Title

- **Status**: Accepted | Superseded by NNNN | Deprecated
- **Date**: YYYY-MM-DD
- **Commit**: <short sha> (if applicable)

## Context
What forced the decision. The constraints, the prior state.

## Decision
What we chose.

## Consequences
What becomes easier, what becomes harder, what we gave up.

## Alternatives considered
One line each, with why they were rejected.
```

Never edit an accepted ADR — if the decision is later reversed, write a new ADR that supersedes it.
