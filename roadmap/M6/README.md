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

## Dependencies

- S6.0 (prompt-injection scanner) lands before S6.1 -- web RAG is the first ingress for untrusted text and should not run unscanned.
- S6.4 (remote auth) requires S6.3 (Crow server).
- S6.5 (browser frontend) requires S6.3 and is unlocked further by S6.4 for cross-device use.
- S6.6 (VS Code shim) requires S6.3's HTTP endpoints.
- S6.2 (ZIM) is independent and can land any time.
- S6.7 (skills) requires M4 [S4.G](../M4/S4.G-mcp.md) (MCP) and [S4.X](../M4/S4.X-prompt-templates.md) (Prompt Templates); the install CLI is unlocked further by S6.5 for the public skill registry.
- S6.8 (delta archives) requires M5 [S5.F](../M5/S5.F-compaction-v2.md) (snapshot archive chain to migrate from) and is unlocked further by [S5.I](../M5/S5.I-tabs-and-sessions.md) (Manage Sessions dialog hosts the per-session archive viewer). Storage motivation strengthens with S6.4 / S6.5 (long-lived remote / cross-device sessions).
- S6.9 (macOS port) is independent and can land any time. Internally split into Stage A (build & launch), Stage B (shell + MCP parity), Stage C (polish & distribution); each is shippable on its own.
