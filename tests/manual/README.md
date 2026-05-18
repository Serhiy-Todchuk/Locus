# Manual Tests

Step-by-step test plans for QA-style verification of user-facing features.
Each document is named for the feature as users see it -- not the internal
stage code -- so non-developers can find the right plan without reading the
roadmap.

These complement, but don't replace, the automated suites:

- **Unit tests** (`tests/test_*.cpp`) -- fast, hermetic, run on every build.
- **Integration tests** (`tests/integration/test_int_*.cpp`) -- live LLM, manual to
  run, but still scripted assertions.
- **UI automation tests** (`tests/ui_automation/`, S5.L) -- scripted Windows UIA
  driver. Covers the smoke + settings-tour baselines from a subprocess; new
  manual plans here can focus on what UIA can't structurally reach.
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
| [memory-bank-panel.md](memory-bank-panel.md) | Memory Bank Panel (S5.K) | View > Memory Bank (Ctrl+M) dockable panel: list / filter / edit / bulk-ops / soft-delete / restore round-trip, live updates from `/memorize` + `/forget`, capability-toggle off-switch. |
| [plan-mode.md](plan-mode.md) | Plan mode | Chat / Plan / Execute switcher, plan bubble Approve/Reject, multi-tool execute stress test. |
| [gitignore.md](gitignore.md) | `.gitignore` respect | Root + nested patterns excluded from index, mid-session `.gitignore` edit reload, `respect_gitignore=false` off-switch. |
| [auto-commit.md](auto-commit.md) | Per-turn auto-commit | Happy path on current branch, dedicated agent branch (auto-create), no-git fallback (silent no-op), commit failure surfacing. |
| [prompt-templates.md](prompt-templates.md) | Prompt templates | Project + global `.locus/prompts/` and `%APPDATA%\Locus\prompts\` round-trip, `/reload`, frontmatter rendering in `/help`, missing-reference literal render, built-in / tool precedence, half-context warning, unknown-slash error. |
| [at-mentions.md](at-mentions.md) | `@`-mention file references | Type-ahead popup over indexed paths, Tab/Enter insertion, submit-time auto-attach for the first resolved path, email-style `@` suppression, trailing-punctuation handling. |
| [workspace-trust.md](workspace-trust.md) | Workspace tool trust + outside-workspace warning | Settings -> Tool Approvals per-tool Auto/Ask/Deny dropdowns, friction confirm on first Auto for mutating tools, run_command outside-workspace path detection re-triggers the approval panel with a yellow banner. |
| [terminal-panel.md](terminal-panel.md) | Live Terminal pane (S5.B) | View > Terminal (Ctrl+`) toggle, sync `run_command` live streaming + ANSI colour + cmake/ninja progress overwrite, per-bg-process tabs with running/killed/exited badges, scrollback cap, auto-scroll stick-to-bottom. |
| [capabilities.md](capabilities.md) | Workspace Capabilities (S5.A) | First-open Workspace Capabilities modal, Settings -> Capabilities tab, end-to-end manifest pruning for the five buckets, failure-mode activity-log surface when the LLM calls a disabled tool, legacy-config migration. |
| [tool-approval.md](tool-approval.md) | Tool approval modal (S5.Z #3) | Modal Approve/Reject/X-close round trip, edit_file diff view + Modify/Confirm, multiple sequential approvals in one turn, no leftover dock pane. |
| [global-settings.md](global-settings.md) | Global settings template + ~/.locus (S5.M) | First-open seed from `~/.locus/config.json`, Save-as-defaults round-trip, Open Global Config menu, legacy %APPDATA% migration. |
| [inline-diffs-in-chat.md](inline-diffs-in-chat.md) | Inline diffs in chat (S5.C) | `edit_file` per-edit hunks, `write_file` real before/after via dtl, `delete_file` one-line summary, line-cap truncation, failure suppression. |
| [tool-safety.md](tool-safety.md) | Tool & filesystem safety hardening (S5.O) | Outside-workspace path rejection (sibling-prefix case), approval keyed by `call_id` (stale click drop), atomic `write_file` under forced kill. |
| [compaction.md](compaction.md) | Compaction v2 (S5.F) | Manual Compact dialog (layer checkboxes + live preview), `/compact [instructions]` slash, auto-compact band + heuristics + history-archive chain + footnote. |
| [chat-activity-restructure.md](chat-activity-restructure.md) | Chat / Activity restructure (S5.G) | Collapsible system-prompt bubble + token-cost breakdown, hover-reveal per-message X delete with confirm, embedding-queue activity rows (enqueue + throttled progress + drain). |
| [chat-syntax-highlighting.md](chat-syntax-highlighting.md) | Chat syntax highlighting | Bundled Prism renders code blocks across languages, works offline, degrades to plain monospace when `resources/prism/` is missing. |
| [permission-presets.md](permission-presets.md) | Permission Presets (S5.S) | Settings -> Tool Approvals preset dropdown writes a per-tool snapshot; chat-footer dropdown applies a runtime override; settings save resets runtime overrides; AFK-autonomous combo with auto-compaction; `ask_user` still pauses at "Allow all". |
| [notification-sounds.md](notification-sounds.md) | Notification sounds | Settings -> Notifications per-event toggles (tool approval / ask-user / turn complete / compaction), distinct Windows system aliases, focus gate, settings persistence. |
| [filter-output.md](filter-output.md) | filter_output tool (S5.Z #8) | Agent-driven regex / substring filter over a prior tool result -- build-log "filter for errors" round-trip, before/after context, max_matches cap + invert, invalid-regex error path, substring-mode literal behaviour. |
