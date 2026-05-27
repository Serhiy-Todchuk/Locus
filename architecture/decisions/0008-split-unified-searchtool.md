# 0008. Split unified `SearchTool` into per-mode tools

- **Status**: Accepted (shipped 2026-05-27)
- **Date**: 2026-05-26
- **Related stage**: [S6.17 -- Compaction Fixes + Tool-Arg Robustness](../../roadmap/M6/S6.17-compaction-fixes-and-tool-robustness.md) Task G

## Context

[`SearchTool`](../../src/tools/search_tools.cpp) is registered as a single tool with a `mode` string discriminator: `text` / `regex` / `symbols` / `ast` / `semantic` / `hybrid`. Each mode delegates internally to a separate per-mode class (`SearchTextTool`, `SearchRegexTool`, etc.) that already exists. The unification was done to expose one tool name to the LLM while keeping the implementation modular.

The 2026-05-25/26 TestLocalVibe2 agentic-mode test run on `qwen/qwen3.6-27b` ([findings.md](../../tests/ui_automation/output/agentic_TestLocalVibe2/findings.md)) exposed three failure classes that all trace back to the mode-discriminator design:

- **F11**: Pass 3 Task 1 asked the model to "use search_symbols". The model invoked `search` with `query="snake.cpp"` -- looking up the literal filename in text mode. It never reached for `mode="symbols"` because, from the lazy-manifest one-line description, the modes weren't visible.
- **F15**: Pass 2 the model passed `"type":"regex"` (alias common in Anthropic-style and other agent SDKs). `SearchTool::execute` reads only `mode`; the unknown key fell through to `nlohmann::json::value("mode", "text")`. The call ran as text-mode FTS5 against the literal string `case \d+:`, returning 1 hit on a query that genuinely had 4 matching lines. No error, no warning -- the model thought it had done a regex search.
- **F20**: Pass 5 the model reached for the separate `filter_output` tool when it needed grep-style behaviour, because the unified `search` tool's purpose did not read as "regex over files" from its summary. Cost an extra describe_tool round before recovery.

Lazy_tool_manifest ([ADR-0007](0007-context-budget-reshape-lazy-manifest-and-profiles.md)) amplifies all three because per-mode parameter schemas aren't surfaced until `describe_tool` is called. The model has to guess the discriminator name from training-time priors and guess the right enum value.

The root is shared: **a tool whose identity depends on a string discriminator inside the args is harder for an LLM to call correctly than N tools named after their actual purpose.** The LLM's training-time priors are about *tool names*, not about arg-value discriminators.

## Decision

Restore each mode as a top-level tool in the registry. The six entries:

- `search_text` -- FTS5 over indexed text content
- `search_regex` -- ECMAScript regex, per-line match, with `max_results` cap
- `search_symbols` -- symbol-table lookup; takes `name` not `query`
- `search_semantic` -- vector-cosine over indexed chunks
- `search_hybrid` -- BM25 + semantic blend with optional rerank
- `search_ast` -- Tree-sitter S-expression queries

The existing per-mode classes (which the unified tool already delegates to) become the direct registry entries. The `SearchTool` dispatcher wrapper is removed.

A one-version backward-compatibility shim translates saved-session calls of `{name: "search", args: {mode: "regex", ...}}` into `{name: "search_regex", args: {...}}` on load. Removed next milestone.

## Consequences

### Easier

- F11 / F15 / F20 are eliminated by construction. No `mode` discriminator means no way to silently drop it, no way to miss its existence, no way to confuse "this tool's purpose" with an adjacent tool.
- Each tool's params are tight to its needs. `search_symbols`'s `name` arg is no longer hidden behind `mode=symbols` + the conditional arg-rename branch in `SearchTool::execute`.
- Per-tool `short_description()` is shorter than the unified description's mode-enumeration. With [S6.17](../../roadmap/M6/S6.17-compaction-fixes-and-tool-robustness.md) Task I in place, each entry becomes `"search_regex(query, path_glob?, max_results=50) -- ECMAScript regex per-line"` -- the model reaches for the right tool by name.
- Existing per-tool tests stay; the dispatch-layer test is dropped.

### Harder

- The lazy manifest has six entries instead of one. Measured cost: per-tool one-liner ~80 chars * 6 = ~480 chars; unified description with mode enum ~500 chars. **Net token cost is roughly even**; some slack either way depending on `short_description()` polish.
- `describe_tool` calls now spread across six schemas instead of one. Each schema is smaller; aggregate exploration cost is comparable.
- One backward-compatibility seam to maintain for one milestone (the `name:"search"` shim in `SessionManager::load`).
- Six per-tool tests instead of one parametrised test (the test classes already exist; just split the parametrised cases).

### Gave up

- The "single search entry point" mental model for callers reading source. The trade is: callers gain six clearer names; the LLM gains six obviously-distinct tools; humans reading `search_tools.cpp` lose one layer of indirection (no dispatcher) but gain six smaller files of similar shape.

## Alternatives considered

- **Keep unified, fix the silent-fallback only ([S6.17](../../roadmap/M6/S6.17-compaction-fixes-and-tool-robustness.md) Task D)**. Rejected. Task D solves F15 (alias arg suggestion) but does nothing for F11 (model didn't pass *any* mode arg and got text-mode by default) or F20 (model reached for the wrong tool entirely). Those are about discoverability, not arg-shape correction.
- **Keep unified, but expand `short_description()` to list all six modes inline**. Tried in draft for S6.17 Task I; the unified description balloons to 200+ chars including the mode list AND still requires the model to learn the discriminator. The per-tool form is shorter total and removes the discriminator step.
- **Keep unified, surface modes as separate tools via aliases in the registry** (`search`, `search_text`, `search_regex` all dispatch to the same class with different default `mode` args). Rejected. Adds a third concept ("alias") to the registry and doesn't address F11/F20 -- the model still sees `search` and reaches for it ambiguously. The aliasing is internal complexity for no LLM-visible gain.
- **Split, but only for the high-confusion modes (text/regex/symbols), keep semantic/hybrid/ast under unified `search`**. Rejected. Splits the mental model the wrong way -- the rarely-used modes are the ones that most benefit from clear naming (the model has weaker priors on them and is more likely to miss them).
