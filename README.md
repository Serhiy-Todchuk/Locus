# Locus — Local LLM Agent Assistant

> Locus - an open-source, local workspace AI assistant built for privacy and efficiency.
> Privacy first: whether your machine is offline or your data simply cannot leave it, Locus is designed around locally running LLMs - secure, practical, and under your control.
> Efficiency by design: Locus is optimized for limited hardware, with careful use of RAM, CPU, and context window budget.
> Workspace-native: Locus works across any local workspace, from code repositories to personal documents and offline knowledge bases, combining general assistance, coding agent workflows, and robust navigation of large local datasets.
> Flexible by choice: local models come first, but you can also connect server-based LLMs like ChatGPT or Claude when your workflow needs them.

## Project Status

**Stage: M1 — Desktop App** (S1.1 complete, S1.2 next)

## Documentation Index

| Document | Description |
|---|---|
| [CLAUDE.md](CLAUDE.md) | Working guide for Claude Code (AI assistant context) |
| [roadmap.md](roadmap.md) | Full implementation roadmap: milestones → stages → tasks |
| [test-workspaces.md](test-workspaces.md) | Three test workspaces: Locus itself, Wikipedia/Kiwix, personal docs |
| [vision.md](vision.md) | Why this exists — goals, philosophy, target user |
| [DIFFERENTIATORS.md](DIFFERENTIATORS.md) | Who this is for vs existing tools |
| [requirements.md](requirements.md) | Features, capabilities, constraints |
| [architecture/overview.md](architecture/overview.md) | High-level system design, context strategy |
| [architecture/tool-protocol.md](architecture/tool-protocol.md) | ITool interface and approval flow |
| [architecture/workspace-index.md](architecture/workspace-index.md) | The workspace indexing subsystem design |
| [architecture/tech-stack.md](architecture/tech-stack.md) | Technology stack — decided choices and future frontend options |

## How It Works

Locus Core runs as a background process on your PC. It indexes your workspace, connects
to a local LLM, and exposes an agent that can read, write, search, and run commands
inside that folder. Any frontend — desktop app, terminal, browser, or your phone — can
attach to it. Your PC does all the compute; the frontend is just a thin window.

This means you can work on a project from your phone while sitting on the couch —
browse code, ask questions, approve tool calls, kick off builds — with full visibility
into every step the agent takes. Nothing leaves your network.

## Usage

### Prerequisites

- **LLM server**: [LM Studio](https://lmstudio.ai/) or any OpenAI-compatible endpoint
- **Build tools** (if building from source): CMake 4+, Visual Studio 2026, vcpkg

### Quick Start

```
# Start your LLM server (e.g. LM Studio on localhost:1234)

# Point Locus at a workspace folder
locus.exe <workspace_path>

# Options
locus.exe <workspace_path> --endpoint http://127.0.0.1:1234 -verbose
```

Locus indexes the folder, connects to the LLM, and drops you into an interactive session.

### CLI Commands

| Command | Description |
|---|---|
| `/help` | Show available commands |
| `/quit` | Exit Locus |
| `/reset` | Start a fresh conversation |
| `/compact` | Drop tool results to free context space |
| `/history` | Show conversation summary and token count |
| `/save` | Save current session |
| `/sessions` | List saved sessions |
| `/load <id>` | Resume a saved session |

Anything else you type is sent as a message to the agent. The agent can call tools
(read files, search, run commands, etc.) — each tool call is shown to you for approval
before execution.

### Flags

| Flag | Description |
|---|---|
| `--endpoint URL` | LLM server URL (default: `http://127.0.0.1:1234`) |
| `--model NAME` | Model name (default: server default) |
| `--context N` | Override context window size |
| `-verbose` | Trace-level logging to stderr and `.locus/locus.log` |

### Workspace Data

Locus creates a `.locus/` directory inside each workspace:

```
<workspace>/
└── .locus/
    ├── config.json     Workspace settings (exclude patterns, etc.)
    ├── index.db        SQLite index (FTS5 + symbols + headings)
    ├── sessions/       Saved conversation sessions
    └── locus.log       Rotating log file
```

Add `.locus/` to your `.gitignore`.
