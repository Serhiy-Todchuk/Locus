# <Project> workspace guide

<!--
The LOCUS.md-equivalent: terse rules the agent reads DURING a task (not the full context
file). Keep it to one screen. This is etiquette + guardrails, not design.
-->

You are coding **<Project> itself** -- <one-line what it is>.

## Before editing

1. Read the file first. Never edit without reading.
2. Unfamiliar area? Check the context file (Code Map, Document Map).
3. Search first: <symbol search> for names, <text search> for strings.

## Code rules

- <language / namespace / file layout in one line>
- <naming in one line>
- <where new files register (build system)>
- <test convention in one line>

## Do not

- Add comments unless the *why* is non-obvious (bug, constraint, workaround).
- Write docstrings or block comments.
- Add abstractions, error handling, or fallbacks beyond what the task needs.
- Invent backwards-compat shims. Delete unused code instead.
- Create docs/files unless asked.

## Performance / quality bar

<one line: the owner's hard requirement - instant UI, low RAM, correctness, whatever it is>

## After any stage/task

1. Mark `[x]` in the relevant roadmap file.
2. Update the context file's "Current Stage".
3. Non-trivial architectural shift -> add an ADR + update the decisions table.

## Tool etiquette

- Prefer edit over full-file rewrite.
- <the deterministic component the AI never writes>.
- Run tests after non-trivial changes; read the output.

## When stuck

Ask. Do not guess. Verify paths and APIs with search before using them.
