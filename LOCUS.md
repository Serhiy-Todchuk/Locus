# Locus workspace guide

You are coding **Locus itself** — a C++20 workspace-bound LLM agent.

## Before editing

1. `read_file` first. Never edit without reading.
2. Unfamiliar area? Check `CLAUDE.md` (Code Map, Document Map).
3. Agent turn loop? `architecture/agent-loop.md`.
4. Search first: `search_symbols` for names, `search_text` for strings.

## Code rules

- C++20. Namespace `locus`. All code in `src/` (headers + sources together, no `include/`).
- Files `snake_case.h`/`.cpp`. Classes `PascalCase`. Members `snake_case`.
- Tests: `tests/test_<topic>.cpp`, Catch2, ASCII-only names, stage tag like `[s2.4]`.
- New `.cpp` goes in `locus_core` source list in root `CMakeLists.txt`.

## Do not

- Add comments unless the *why* is non-obvious (bug, constraint, workaround).
- Write docstrings or block comments.
- Add abstractions, error handling, or fallbacks beyond what the task needs.
- Invent backwards-compat shims. Delete unused code instead.
- Create `.md` files unless asked.

## Performance

Target: instant UI, low RAM, low CPU. User rejects bloat. Prefer direct code over layers.

## Build + test (Windows MSVC static CRT)

```
cmake --build build --config Debug
build/Debug/locus_tests.exe
build/Debug/locus.exe . -verbose
```

`-verbose` writes trace to `.locus/locus.log`. Read the log after changes.

## After any stage/task

1. Mark `[x]` in the relevant `roadmap/Mx.md` or `roadmap/Mx/Sx.y-*.md`.
2. Update `CLAUDE.md` "Current Stage".
3. Non-trivial architectural shift → add `architecture/decisions/NNNN-title.md` and update
   CLAUDE.md "Key Decisions" table.

## Tool etiquette

- Paginate file reads. No full-file dumps.
- Prefer `edit` over `write_file` for existing files.
- Index is never written by LLM — deterministic updates only.
- Run tests after non-trivial changes, read the output.

## When stuck

Ask the user. Do not guess. Verify paths and APIs with `search_symbols` / `list_directory`
before using them.
