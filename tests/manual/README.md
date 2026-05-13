# Manual Tests

Step-by-step test plans for QA-style verification of user-facing features.
Each document is named for the feature as users see it -- not the internal
stage code -- so non-developers can find the right plan without reading the
roadmap.

These complement, but don't replace, the automated suites:

- **Unit tests** (`tests/test_*.cpp`) -- fast, hermetic, run on every build.
- **Integration tests** (`tests/integration/test_int_*.cpp`) -- live LLM, manual to
  run, but still scripted assertions.
- **Manual tests** (this folder) -- click-by-click, eye-on-screen verification
  of UI behavior, error paths, and end-to-end workflows that scripted tests
  can't reasonably cover (real third-party MCP servers, drag-and-drop flows,
  visual layout regressions, ...).

If you spot a feature without a manual test plan and you can describe how
you'd test it by hand, please write one -- the bar is low: one Markdown file,
numbered steps, expected results per step.

## Index

| Document | Feature | Scope |
|---|---|---|
| [mcp.md](mcp.md) | MCP servers (Model Context Protocol) | Loading `.locus/mcp.json`, Settings panel, trust toggle, restart, error paths, end-to-end LLM call. |
| [memory-bank.md](memory-bank.md) | Memory Bank | `/memorize` + `/forget` round-trip, on-disk YAML format, persistence + auto-injected prompt slot, `add_memory` approval pane, `memory.enabled=false` kill switch. |
| [plan-mode.md](plan-mode.md) | Plan mode | Chat / Plan / Execute switcher, plan bubble Approve/Reject, multi-tool execute stress test. |
| [gitignore.md](gitignore.md) | `.gitignore` respect | Root + nested patterns excluded from index, mid-session `.gitignore` edit reload, `respect_gitignore=false` off-switch. |
| [auto-commit.md](auto-commit.md) | Per-turn auto-commit | Happy path on current branch, dedicated agent branch (auto-create), no-git fallback (silent no-op), commit failure surfacing. |
| [prompt-templates.md](prompt-templates.md) | Prompt templates | Project + global `.locus/prompts/` and `%APPDATA%\Locus\prompts\` round-trip, `/reload`, frontmatter rendering in `/help`, missing-reference literal render, built-in / tool precedence, half-context warning, unknown-slash error. |
| [at-mentions.md](at-mentions.md) | `@`-mention file references | Type-ahead popup over indexed paths, Tab/Enter insertion, submit-time auto-attach for the first resolved path, email-style `@` suppression, trailing-punctuation handling. |
| [workspace-trust.md](workspace-trust.md) | Workspace tool trust + outside-workspace warning | Settings -> Tool Approvals per-tool Auto/Ask/Deny dropdowns, friction confirm on first Auto for mutating tools, run_command outside-workspace path detection re-triggers the approval panel with a yellow banner. |
