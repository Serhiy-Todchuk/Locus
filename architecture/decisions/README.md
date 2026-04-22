# Architecture Decision Records

Short, dated records explaining **why** non-trivial architectural changes were made.

The [Key Decisions](../../CLAUDE.md#key-decisions-made) table in CLAUDE.md is a *snapshot* of the
current choices. ADRs are the *history* — they preserve the reasoning behind pivots so nobody
has to re-derive it six months later.

## When to write one

Write an ADR when:

- A previously-committed technology or approach is replaced (ONNX → llama.cpp, ImGui → wxWidgets)
- The shape of a subsystem changes (one DB file → two)
- A decision rejects an obvious alternative for a non-obvious reason
- Something would otherwise make a future reader ask "why is it this way?"

Do **not** write an ADR for routine implementation choices that are already obvious from the code,
or for decisions already fully captured in `tech-stack.md` with rationale.

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
