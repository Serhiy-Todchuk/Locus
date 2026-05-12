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
