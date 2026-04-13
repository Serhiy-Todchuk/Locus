# Locus — Local LLM Agent Assistant

> A workspace-bound AI assistant built for effective utilization of local LLMs,
> token-efficient operation, and robust navigation of large local datasets.

## Project Status

**Stage: Pre-planning / Architecture**

## Documentation Index

| Document | Description |
|---|---|
| [CLAUDE.md](CLAUDE.md) | Working guide for Claude Code (AI assistant context) |
| [test-workspaces.md](test-workspaces.md) | Three test workspaces: Locus itself, Wikipedia/Kiwix, personal docs |
| [vision.md](vision.md) | Why this exists — goals, philosophy, target user |
| [DIFFERENTIATORS.md](DIFFERENTIATORS.md) | Who this is for vs existing tools |
| [requirements.md](requirements.md) | Features, capabilities, constraints |
| [architecture/overview.md](architecture/overview.md) | High-level system design, context strategy |
| [architecture/tool-protocol.md](architecture/tool-protocol.md) | ITool interface and approval flow |
| [architecture/workspace-index.md](architecture/workspace-index.md) | The workspace indexing subsystem design |
| [architecture/tech-candidates.md](architecture/tech-candidates.md) | Technology stack decisions and rationale |
| [naming.md](naming.md) | Name decision rationale |

## One-Line Summary

An app (later: VS Code plugin and beyond) that binds an LLM agent to a local folder,
gives it efficient tools to navigate and act on that folder's contents, and puts the
user in full transparent control of the process — all without an internet connection.
