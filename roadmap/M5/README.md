# M5 — Connected

**Goal**: Core accessible over LAN. VS Code sends editor context. Browser frontend works. Wikipedia works end-to-end.

**Test workspaces unlocked**: All, from any device.

## Stages

| Stage | Title |
|---|---|
| [S5.1](S5.1-web-retrieval.md) | Web Retrieval (RAG) |
| [S5.2](S5.2-zim-reader.md) | ZIM Reader (Wikipedia / Kiwix) |
| [S5.3](S5.3-crow-frontend.md) | CrowServer Frontend |
| [S5.4](S5.4-remote-access.md) | Remote Access |
| [S5.5](S5.5-web-frontend.md) | Web / Browser Frontend |
| [S5.6](S5.6-vscode-shim.md) | VS Code Shim |

## Dependencies

- S5.4 (remote auth) requires S5.3 (Crow server).
- S5.5 (browser frontend) requires S5.3 and is unlocked further by S5.4 for cross-device use.
- S5.6 (VS Code shim) requires S5.3's HTTP endpoints.
- S5.2 (ZIM) is independent and can land any time.
