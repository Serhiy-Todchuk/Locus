# Locus

<p align="center">
  <img src="assets/icons/github/locus-github-front-1024.png" alt="Locus app icon" width="180">
</p>

**A local AI agent for your workspace.**

Locus is a local-first coding and knowledge-base agent for people who want AI help
without uploading their code, documents, or private notes to a cloud service.

It indexes your workspace, connects to a local OpenAI-compatible LLM server, and gives
the model tools to search, edit, run commands, remember facts, and explain what it
finds. Tool calls, context use, activity, diffs, metrics, and undo points are surfaced
so the agent's workflow stays inspectable instead of mysterious.

Unlike cloud-first agents retrofitted for local models, Locus is designed around the
constraints of consumer hardware: smaller context windows, slower inference, limited
VRAM, and models that need clear tools instead of giant pasted prompts.

## Why Locus

- **Your workspace stays local.** Indexes, embeddings, sessions, memory, logs, and
  checkpoints live on your machine.
- **Built for local LLMs.** Locus uses indexed retrieval, semantic search, reranking,
  context budgeting, compaction, and lean tool manifests so smaller models spend less
  time guessing and more time using the right context.
- **Transparent by default.** You can see what context the agent is using, what tools
  it wants to call, what changed, what it ran, how long it took, and what can be
  undone.
- **It understands more than code.** Search across repositories, PDFs, DOCX, XLSX,
  Markdown, HTML, JSON/YAML, and offline knowledge bases.
- **It can act, but you stay in control.** File edits, deletes, shell commands, and
  MCP tools go through visible approval policies.
- **It has a working desktop surface.** The Windows GUI and CLI share the same
  core: indexing, semantic search, undo, plan mode, metrics, memory, MCP, and
  background commands.

## Project Status

**Current stage: M5 -- Polish, UX & Performance** (in progress)

| Milestone | Status | Notes |
|---|---|---|
| M0 -- CLI Prototype | Complete | CLI, workspace, index, LLM client, tools, agent loop |
| M1 -- Desktop App | Complete | wxWidgets GUI, chat, tool approval, settings, tray |
| M2 -- Full Workspace Support | Complete | semantic search, document extraction, attachment flow |
| M3 -- Refactoring | Complete | split agent/index/tools/GUI structure, threading docs, ADRs |
| M4 -- Agent Quality | Complete | diff editing, undo, plan mode, MCP, memory, git, retrieval quality |
| M5 -- Polish, UX & Performance | In progress | UI automation, terminal panel, capability toggles, global settings, inline diffs, LLMContext refactor landed; context-management polish still active |
| M6 -- Connected | Planned | remote access, VS Code shim, web frontend, ZIM reader |

Two binaries ship from the same codebase:

- `locus.exe` -- terminal CLI
- `locus_gui.exe` -- native Windows desktop app (wxWidgets + WebView2)

## Platform Support

Locus is currently developed and tested on **Windows 11**. The core is written
with cross-platform support in mind, but several runtime integrations are still
Windows-first: shell execution, background process management, MCP stdio
transport, autostart, recent-workspace integration, and the GUI automation
harness. macOS and Linux support are planned, not production-ready.

## Documentation Index

| Document | Description |
|---|---|
| [CONTRIBUTING.md](CONTRIBUTING.md) | Build instructions, code style, full per-target / per-config command reference |
| [CLAUDE.md](CLAUDE.md) | Working guide for Claude Code (AI assistant context, build commands, code map) |
| [roadmap.md](roadmap.md) | Full implementation roadmap: milestones -> stages -> tasks |
| [test-workspaces.md](test-workspaces.md) | Three test workspaces: Locus itself, Wikipedia/Kiwix, personal docs |
| [vision.md](vision.md) | Why this exists -- goals, philosophy, target user |
| [DIFFERENTIATORS.md](DIFFERENTIATORS.md) | Who this is for vs existing tools |
| [requirements.md](requirements.md) | Features, capabilities, constraints |
| [architecture/overview.md](architecture/overview.md) | High-level system design, context strategy |
| [architecture/diagrams.md](architecture/diagrams.md) | Visual map -- eight Mermaid diagrams covering agent loop, index pipeline, LLM stack, threading, compaction, edit safety |
| [architecture/tool-protocol.md](architecture/tool-protocol.md) | ITool interface and approval flow |
| [architecture/workspace-index.md](architecture/workspace-index.md) | The workspace indexing subsystem design |
| [architecture/tech-stack.md](architecture/tech-stack.md) | Technology stack -- decided choices and future frontend options |
| [tests/integration/README.md](tests/integration/README.md) | Manual end-to-end tests that drive `AgentCore` against a live local LLM |
| [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md) | Retrieval quality benchmark (recall@K, MRR, nDCG) |

## What Works Today

The current codebase is a working local-agent application:

- **Agent runtime**: streaming conversations, tool-call parsing across OpenAI-style
  and XML-style model outputs, approval gates, plan mode, slash commands, sessions,
  layered context compaction (manual via dialog or fully automatic with per-workspace
  defaults configurable via Save), file-change awareness, and per-turn activity logging.
- **Safe workspace operations**: read-before-edit discipline, app-level checkpoints
  and `/undo`, inline diff rendering, delete/write/edit tools, command execution,
  background processes, and outside-workspace shell-path warnings.
- **Workspace intelligence**: SQLite FTS5 indexing, document extraction, symbols,
  headings, AST/regex search, optional local embeddings, hybrid retrieval,
  reranking, and a retrieval evaluation harness.
- **Extensibility and memory**: MCP client support, prompt templates, model presets,
  sampler controls, workspace-scoped memory with search, and git-aware workflows
  including auto-commit support.
- **Desktop polish in M5**: terminal panel, capability toggles, global settings,
  inline diffs in chat, UI automation scripts, metrics view, and the `LLMContext`
  refactor that centralizes conversation-facing state.

## How It Works

Locus runs against one local workspace folder at a time. It builds a search index,
stores per-workspace state under `.locus/`, and exposes tools the model can call
inside that folder. Mutating tools are visible in the activity stream and pass
through approval policies before execution.

Your PC does the local work: indexing, embeddings, reranking, tool execution,
sessions, checkpoints, memory, and logs. Nothing leaves your network unless you
point Locus at a remote LLM endpoint yourself.

## First-time setup on a new PC

End-to-end checklist for getting Locus running on a fresh Windows 11 machine.

### 1. Install the LLM server

Download and run [LM Studio](https://lmstudio.ai/). In the app, search for and
download a tool-calling-capable model -- recommended starters:

- **Gemma 4 E4B** (lightweight, ~3 GB, 8k context) -- good first model.
- **Qwen 3 14B / 32B** -- stronger, needs more VRAM.
- **GPT-OSS 20B** -- larger, very capable.

In LM Studio: load the model, click *Start Server* (default port `1234`).
Locus will auto-detect the loaded model and its context length.

### 2. Get Locus

#### Option A: Download a pre-built bundle (when releases are published)

Unzip the bundle anywhere. The folder layout is:

```
locus/
  locus.exe
  locus_gui.exe
  pdfium.dll
  resources/
    prism/        (bundled syntax highlighter for chat code blocks)
  models/         (initially empty -- populate via the script in step 3)
```

#### Option B: Build from source

Install:

- **Visual Studio 2026** (C++ workload + Windows SDK)
- **CMake 4.0+**
- **vcpkg** -- typically at `C:/vcpkg`. After cloning, run `bootstrap-vcpkg.bat`.

Then from the repo root:

```
cmake -B build/release -G "Visual Studio 18 2026" ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets

cmake --build build/release --config Release
```

The first configure pulls every dependency through vcpkg in static-CRT mode and
fetches Tree-sitter grammars -- plan for **10-25 minutes**. Subsequent builds
finish in seconds. Outputs land at `build/release/Release/locus.exe` and
`build/release/Release/locus_gui.exe`. `pdfium.dll` and the bundled Prism
assets under `resources/prism/` are auto-copied next to the exes by POST_BUILD steps.

Full build/test reference: [CONTRIBUTING.md](CONTRIBUTING.md).

### 3. Download embedding + reranker models

Locus uses a local embedding model (semantic search) and an optional cross-encoder
reranker (top-K precision). Both are GGUF files placed in `models/` next to
`locus.exe`.

| Script | Profile | Files | Total |
|---|---|---|---|
| [models/download.ps1](models/download.ps1) | Recommended (multilingual) | `bge-m3` Q8 + `bge-reranker-v2-m3` Q8 | ~1.27 GB |
| [models/download-small.ps1](models/download-small.ps1) | Small / English-only | `bge-small-en-v1.5` Q8 + `ms-marco-MiniLM-L6-v2` Q4_K_M | ~58 MB |

The small reranker is ~13x faster than the recommended one in practice
(22M vs 568M params; ~8 ms vs ~100 ms per rerank in Release on CPU) but
English-only and weaker on long passages -- pick by your corpus.

```powershell
# From the repo root. -ExecutionPolicy Bypass avoids permanently changing
# your machine's policy for one-off scripts.
powershell -NoProfile -ExecutionPolicy Bypass -File .\models\download.ps1

# Embedder only / reranker only
powershell -NoProfile -ExecutionPolicy Bypass -File .\models\download.ps1 -Embedder
powershell -NoProfile -ExecutionPolicy Bypass -File .\models\download.ps1 -Reranker

# Smaller English-only profile
powershell -NoProfile -ExecutionPolicy Bypass -File .\models\download-small.ps1
```

Files are skipped if already present at the expected size; pass `-Force` to
re-download. Scripts pull from the official `ggml-org` and `gpustack` Hugging
Face mirrors -- no auth required.

### 4. Run Locus

```
# GUI (recommended for desktop use)
locus_gui.exe                            # opens a workspace picker
locus_gui.exe D:\path\to\my\workspace    # opens that folder directly

# CLI
locus.exe D:\path\to\my\workspace
```

Locus indexes the folder, connects to LM Studio, and drops you into an
interactive session. The first index build runs in the foreground; embeddings
populate in the background after that.

### 5. Per-workspace state

On first open, Locus creates a `.locus/` directory inside the workspace:

```
<workspace>/
  .locus/
    config.json     # workspace settings (LLM endpoint, exclude patterns, timeout, ...)
    index.db        # SQLite index (FTS5 + symbols + headings)
    vectors.db      # SQLite + sqlite-vec embedding store (optional)
    sessions/       # saved conversation sessions
    checkpoints/    # pre-mutation snapshots for /undo
    locus.log       # rotating log file
```

Add `.locus/` to your `.gitignore`. The config file is human-editable -- see the
LLM, index, and agent sections inside `config.json` for tunables (stall timeout,
context limit, exclude patterns, reranker enable, etc.).

## CLI Commands

Type any of these inside an open session:

| Command | Description |
|---|---|
| `/help` | Show available commands |
| `/quit` | Exit Locus (CLI only) |
| `/reset` | Start a fresh conversation |
| `/compact` | Drop tool results to free context space |
| `/history` | Show conversation summary and token count |
| `/save` | Save current session |
| `/sessions` | List saved sessions |
| `/load <id>` | Resume a saved session |
| `/undo [turn_id]` | Revert files mutated by the most recent turn (0 = most recent) |
| `/metrics` | Show per-turn timing + per-tool histogram |
| `/export_metrics [json\|csv]` | Write metrics under `.locus/metrics/` |

Anything else you type is sent as a message to the agent. The agent can call
tools (read files, search, run commands, etc.) -- each tool call is shown to
you for approval before execution.

## Flags

Both `locus.exe` and `locus_gui.exe` accept:

| Flag | Description |
|---|---|
| `--endpoint URL` | LLM server URL (default: `http://127.0.0.1:1234`) |
| `--model NAME`   | Model name (default: server default, auto-detected from LM Studio) |
| `--context N`    | Override context window size |
| `-verbose`       | Trace-level logging to stderr and `<workspace>/.locus/locus.log` |

`-verbose` is the right flag any time something looks wrong -- the rotating
log file then captures every SQL query, tool call, and LLM token.

## Troubleshooting

**"Cannot connect to LLM server"** -- LM Studio isn't running, or its server
isn't started. Open LM Studio, load a model, click *Start Server*.

**"LLM stream stalled after retry (no data for 600s)"** -- a local LLM took
longer than 600 s to send the first byte. Common with large models on a fresh
prompt where prefill alone takes minutes. Raise the watchdog in
`<workspace>/.locus/config.json`:

```json
"llm": {
  "timeout_ms": 600000
}
```

**"Semantic search enabled but model 'bge-m3-Q8_0.gguf' not found"** -- you
haven't run the model download script yet, or the `models/` folder isn't next
to the exe. The exe walks up to 6 parent dirs looking for `models/`, so the
dev layout (exe in `build/release/Release/`, models at repo root) works
without copying.

**Workspace lock errors** -- another Locus process already has the workspace
open, or a previous run crashed and the lock file wasn't released. Locus
releases the lock on clean exit and the OS releases it on crash. If you see
this on a fresh run, manually delete `<workspace>/.locus/locus.lock`.

## License

MIT.
