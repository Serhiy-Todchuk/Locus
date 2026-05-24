# M6 -- Connected & Misc

**Goal**: Core accessible over LAN. VS Code sends editor context. Browser frontend works. Wikipedia works end-to-end. Plus the non-connectivity work that doesn't fit M5's "polish" frame (storage migrations, on-demand skills, future expansions) -- the bucket is no longer narrowly themed.

**Test workspaces unlocked**: All, from any device.

**Note**: This milestone was previously numbered M5 (the "Connected" milestone). It was renumbered to M6 when a new M5 ("Polish, UX & Performance") was inserted ahead of it, and renamed to "Connected & Misc" once it became clear additional non-connectivity tasks would land here (e.g. S6.8 delta-history archives). Stage IDs were renumbered S5.x -> S6.x at the same time. File-level link stability is preserved within the new layout (all `S6.x-*.md` filenames match the new IDs).

## Stages

- [ ] **S6.0** -- Prompt-Injection Scanner (5-category: instruction override, role impersonation, exfiltration, encoding evasion, social eng) -- lands before S6.1 so untrusted HTML pulled by web RAG is vetted on ingress. Stage doc TBD.
- [ ] **[S6.1](S6.1-web-retrieval.md)** -- Web Retrieval (RAG)
- [ ] **[S6.2](S6.2-zim-reader.md)** -- ZIM Reader (Wikipedia / Kiwix)
- [ ] **[S6.3](S6.3-crow-frontend.md)** -- CrowServer Frontend
- [ ] **[S6.4](S6.4-remote-access.md)** -- Remote Access
- [ ] **[S6.5](S6.5-web-frontend.md)** -- Web / Browser Frontend
- [ ] **[S6.6](S6.6-vscode-shim.md)** -- VS Code Shim
- [ ] **[S6.7](S6.7-skills.md)** -- Skills (On-Demand Capability Packs)
- [ ] **[S6.8](S6.8-delta-history-archives.md)** -- Delta History Archives (replace S5.F snapshot chain with per-compaction deltas + chain walker)
- [ ] **[S6.9](S6.9-macos-port.md)** -- macOS Port (CMake fork + POSIX process/MCP branches + .app bundle, three sub-stages A/B/C)
- [ ] **[S6.10](S6.10-small-model-robustness.md)** -- Small-Model Robustness Pass (8 tasks: JSON repair pre-pass for tool-call bodies / quality monitor with 4-detector auto-correction / strip past-turn thinking from LLM payload / server-side grammar-constrained decoding / few-shot examples in tool descriptions / auto-detect model + auto-apply preset / anti-truncation detection for code writes / per-model sampler defaults)
- [x] **[S6.11](S6.11-lazy-tool-manifest.md)** -- Lazy Tool Manifest (per-turn tool schemas trimmed to name+description summaries; new `describe_tool(name)` meta-tool returns full schema on demand; closes the per-turn 2854 t API-manifest + ~1k t system-prompt-tools sink for local-LLM 16k-context budgets) **done 2026-05-25**
- [x] **[S6.12](S6.12-system-prompt-profiles.md)** -- System-Prompt Profiles (Full / Compact / Minimal via `system_prompt.profile` setting; trims the prose Rules / Editing / Shell / MSVC sections without losing load-bearing invariants; lazy-manifest-compatible) **done 2026-05-25**
- [x] **[S6.13](S6.13-reasoning-watchdog.md)** -- Reasoning Watchdog + Commit-now button (per-round budget on reasoning seconds / chars, OR semantics; `agent.reasoning_auto_nudge` cancels the in-flight LLM stream and injects a "Stop reasoning, commit now" steering message; non-modal `locus.chat.commit_btn` in the footer surfaces when manual mode is on and the watchdog trips; 2-nudge cap before turn aborts with "Agent appears stuck") **done 2026-05-25**
- [ ] **[S6.14](S6.14-thinking-knob.md)** -- Thinking ON/OFF/auto Knob in LLM Settings (tri-state `enable_thinking` per workspace; per-model-family injection: Qwen3 `chat_template_kwargs.enable_thinking`, Qwen2 `/no_think`, o1 `reasoning_effort`; moved from backlog 2026-05-25)

## Dependencies

- S6.0 (prompt-injection scanner) lands before S6.1 -- web RAG is the first ingress for untrusted text and should not run unscanned.
- S6.4 (remote auth) requires S6.3 (Crow server).
- S6.5 (browser frontend) requires S6.3 and is unlocked further by S6.4 for cross-device use.
- S6.6 (VS Code shim) requires S6.3's HTTP endpoints.
- S6.2 (ZIM) is independent and can land any time.
- S6.7 (skills) requires M4 [S4.G](../M4/S4.G-mcp.md) (MCP) and [S4.X](../M4/S4.X-prompt-templates.md) (Prompt Templates); the install CLI is unlocked further by S6.5 for the public skill registry.
- S6.8 (delta archives) requires M5 [S5.F](../M5/S5.F-compaction-v2.md) (snapshot archive chain to migrate from) and is unlocked further by [S5.I](../M5/S5.I-tabs-and-sessions.md) (Manage Sessions dialog hosts the per-session archive viewer). Storage motivation strengthens with S6.4 / S6.5 (long-lived remote / cross-device sessions).
- S6.9 (macOS port) is independent and can land any time. Internally split into Stage A (build & launch), Stage B (shell + MCP parity), Stage C (polish & distribution); each is shippable on its own.
- S6.10 (small-model robustness) is independent and can land any time. All three sub-features (JSON repair / quality monitor / thinking strip) hook into surfaces that already exist (stream decoders, ToolDispatcher between-round seam, LLMContext payload assembly); no other stage blocks it and it blocks none.
- S6.11 / S6.12 are tightly coupled (one ADR covers both) but otherwise independent of the rest of M6. S6.12 lands after S6.11 because the system-prompt profile cuts share the lazy-manifest plumbing.
- S6.13 (reasoning watchdog) is independent of S6.11-S6.12 -- it operates at the stream layer rather than the prompt layer. Companion to S6.14 (one caps reasoning, the other disables it at source). Either can land first.
- S6.14 (thinking knob) is independent of everything else in M6. Cheap to land (~one new field + one extra_body branch + one Settings row).
