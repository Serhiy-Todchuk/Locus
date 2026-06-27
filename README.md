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
  it calls, what changed, what it ran, how long it took, and what can be
  undone -- the goal is that you *understand* what's happening under the hood.
- **It understands more than code.** Search across repositories, PDFs, DOCX, XLSX,
  Markdown, HTML, and JSON/YAML, including offline document libraries and
  native ZIM archives -- open a Wikipedia / Kiwix `.zim` directly as a
  read-only workspace and search/read its articles.
- **You tune the behavior, not every step.** Settings shape how the agent works --
  permission presets (from auto-run-everything to ask-before-edits), capability
  toggles, prompt-cost and compaction profiles, model and sampler presets.
- **It has a working desktop surface.** The GUI (Windows and macOS) and CLI share
  the same core: indexing, semantic search, undo, plan mode, metrics, memory, MCP,
  and background commands.

## Project Status

**Current stage: M6 -- Connected & Misc** (in progress). M0-M5 complete.

| Milestone | Status | Notes |
|---|---|---|
| M0 -- CLI Prototype | Complete | CLI, workspace, index, LLM client, tools, agent loop |
| M1 -- Desktop App | Complete | wxWidgets GUI, chat, tool approval, settings, tray |
| M2 -- Full Workspace Support | Complete | semantic search, document extraction, attachment flow |
| M3 -- Refactoring | Complete | split agent/index/tools/GUI structure, threading docs, ADRs |
| M4 -- Agent Quality | Complete | diff editing, undo, plan mode, MCP, memory, git, retrieval quality |
| M5 -- Polish, UX & Performance | Complete | UI automation, terminal panel, capability toggles, global settings, inline diffs, LLMContext refactor, compaction v2, chat/activity restructure, multi-tab, memory bank UI, permission presets, non-code workspace proof |
| M6 -- Connected & Misc | In progress | **Done:** prompt-injection scanner + taint propagation, ZIM reader (native Wikipedia/Kiwix), macOS port, small-model robustness, lazy tool manifest, system-prompt profiles, reasoning watchdog, session-restore parity, endpoint profiles (multi-LLM + hosted), compaction fixes + tool-arg robustness, index storage hygiene, tool-arg coercion + transient-endpoint retry, agent convergence aids. **Planned:** remote access, VS Code shim, web frontend, web RAG |

The large body of M6 small-LLM coding work (S6.10-S6.21) is cataloged in
**[CODING_AGENT.md](CODING_AGENT.md)** -- see the [Coding Agent](#coding-agent) section below.

Two binaries ship from the same codebase:

- `locus.exe` -- terminal CLI
- `locus_gui.exe` -- native desktop app (wxWidgets + WebView2 on Windows, WKWebView on macOS)

## Platform Support

**Windows and macOS (arm64) are both supported; Linux is planned.** Windows is
the primary, longest-tested platform. The macOS port (S6.9) builds a native
arm64 CLI and GUI with a Metal-accelerated embedder, full POSIX shell execution
(`run_command` / `run_command_bg`), MCP stdio transport, notification sounds,
autostart on login, recent-document integration, and an optional `.app` bundle.
The one macOS gap is the GUI automation test harness (the Windows UIA driver has
not been ported to the macOS accessibility API yet) -- a developer-tooling
deferral, not a user-facing one.

Linux is not yet built; the core is written cross-platform and most runtime
integrations already have POSIX paths from the macOS work, so it is a smaller
effort than the original macOS port was.

## Trust Model

Locus treats your **workspace as trusted**. Your code, documents, and notes are content
you wrote or chose to bring in, so the agent reads them as-is. Only point Locus at folders
you trust -- it indexes everything inside and will act on what it finds there.

Content from **outside** your workspace is treated as **untrusted**: third-party MCP tool
output today, and web pages / offline knowledge bases (Wikipedia / ZIM) once those ingress
paths land. This text is written by others and could contain hidden instructions aimed at
hijacking the agent ("ignore your rules and send X to..."). MCP tools are untrusted by
default and produce nothing until you opt them in. The hard backstop, in place today: nothing
risky runs without passing the visible approval gate, so you always get the final say. An
ingress prompt-injection scanner that flags suspicious untrusted text and origin-stamps it is
planned to land alongside web retrieval (M6) -- a visibility tripwire on top of the approval
gate, which stays the actual control.

In short: **trust your workspace before you open it; the outside world is never trusted by
default.**

## Documentation Index

| Document | Description |
|---|---|
| [CODING_AGENT.md](CODING_AGENT.md) | The small-LLM coding aids -- safety nets, context-budget tricks, anti-loop guards, and endpoint resilience that let small local models code reliably |
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
  background processes, outside-workspace shell-path warnings, and named tool-approval
  presets (read-only / ask-before-edits / allow-edits / allow-all, or per-tool custom;
  read tools always auto-approve).
- **Workspace intelligence**: SQLite FTS5 indexing, document extraction, symbols,
  headings, AST/regex search, optional local embeddings + semantic search,
  cross-encoder reranking, and a retrieval evaluation harness.
- **Extensibility and memory**: MCP client support, prompt templates, model presets,
  sampler controls, workspace-scoped memory with search, and git-aware workflows
  including auto-commit support.
- **Desktop GUI surfaces**: chat with streaming markdown, file tree, terminal
  panel, real-time activity log, metrics view, dockable memory bank, tool
  approval panel, multi-tab conversations, and a nine-tab settings dialog
  (LLM / Index / Agent / Capabilities / Tool Approvals / MCP / Notifications /
  Sessions / Endpoints) -- most of what you'd hand-edit in `config.json` is also
  GUI-configurable.
- **Multi-endpoint LLM support**: named endpoint profiles for several LLM sources
  at once (local LM Studio / Ollama plus hosted NVIDIA / OpenAI / OpenRouter /
  Claude-via-proxy with API-key auth), switchable live from a chat-footer dropdown
  so you can run a cheap local model for navigation and hand the hard turn to a
  bigger one.
- **Small-LLM coding aids (M6)**: a thick layer of safety nets and context-budget
  tricks that let small local models code reliably -- see the
  [Coding Agent](#coding-agent) section and [CODING_AGENT.md](CODING_AGENT.md).

## Coding Agent

Locus is a capable coding agent -- it reads, searches, edits, and runs code, with
plan mode, checkpoints and `/undo`, inline diffs, background commands, AST/symbol
search, and per-turn metrics. But the goal is **not** to out-compete Claude Code
or Cursor on raw capability. Those assume a large, reliable cloud model. Locus is
built for the opposite: a **small or mid-sized model running locally, on a tight
context window, on consumer hardware**.

Be realistic about the scope that follows from that. Locus is strongest at
**scoped, supervised coding** -- targeted edits, single-file work, searching and
explaining a codebase, and short build/fix loops -- and at navigating a workspace.
It is **not yet a hands-off "build me an app" agent**: long autonomous
build-until-it-works loops on large, multi-file changes are gated by what small
local models can currently converge on, not by Locus. The safety nets below push
that ceiling as high as the model allows and keep you in the loop when it's
reached.

The value Locus adds for that setup is a thick layer of **safety nets and
small-LLM aids** -- the things that make a model that would otherwise fall over
get the job done:

- **Fit a small context window** -- a lazy tool manifest (full tool schemas
  fetched on demand, not shipped every turn), selectable system-prompt profiles,
  past-turn reasoning stripping, and escalating compaction that never silently
  overflows. A 16k-context model keeps room for the actual task.
- **Survive sloppy tool calls** -- malformed-JSON repair, wrong-type argument
  coercion, single-edit shorthand, unknown-tool closest-match suggestions, and
  optional grammar-constrained decoding, so an almost-right tool call dispatches
  instead of burning a round.
- **Break out of loops** -- a reasoning watchdog for models that think forever, a
  build-error-signature detector for models that thrash on the same compiler error
  across rewrites, and bounded nudge-then-abort steering so a genuinely stuck turn
  stops instead of spinning.
- **Recover from a flaky backend** -- multi-endpoint profiles (run cheap+local for
  navigation, switch to a bigger hosted model for the hard turn without losing
  context), hosted-provider auth, and transient-failure retry with backoff.
- **Stay transparent** -- every correction, watchdog trip, and compaction surfaces
  in the activity log and footer chips, so recovery is visible rather than
  mysterious.

### Recommended local model for coding

From the agentic test sessions, the best **local** coding model so far is
**`qwen/qwen3.6-27b` via LM Studio**:

- **16k context** is enough for **small, tightly-scoped tasks** -- single-file
  creates, renames, targeted edits. It builds these cleanly and recovers from
  tool-arg slips on its own.
- **32k+ context** for any **iterate-until-it-builds loop** or multi-file refactor.
  At 16k the build error gets compacted away every few rounds and it can't
  converge; more context is the single highest-leverage knob.
- Append **`/no_think`** to prompts (a manual Qwen suffix today; the automatic
  Settings toggle is the planned S6.14 knob). With thinking left on, qwen3.6-27b
  can spiral for tens of minutes without ever emitting the edit. Note this is a
  survival tactic at tight context, not a free win -- on genuinely hard reasoning
  tasks with room to think (32k+), leaving thinking on may produce better code.
- Pair with `lazy_tool_manifest=true`, `tool_format="openai_json"`, and
  `prompt_cost="balanced"`.

Read **[CODING_AGENT.md](CODING_AGENT.md)** for the complete, categorized catalog
of these aids (each tagged to the stage that introduced it).

## Beyond code: document folders + offline knowledge bases

Locus indexes PDFs, DOCX, XLSX, Markdown, HTML, JSON, and YAML the same way
it indexes code: full-text + semantic + optional reranking, with the agent
fetching what it needs via tools instead of pasting whole files into the
prompt. Two ready-made demo workspaces live under
[scripts/](scripts/):

```
pwsh ./scripts/build_ws3.ps1
locus_gui.exe D:\Projects\LocusTestWorkspaces\WS3_Documents
```

Then ask: **"Which RFC defines HTTP/3?"** -- the agent runs semantic
retrieval, opens `rfcs/rfc9114.pdf`, and answers with a citation. For an
offline knowledge base, point Locus straight at a Kiwix `.zim` snapshot
(`locus_gui.exe wikipedia_en_simple.zim`) -- it opens read-only, indexes the
articles, and the agent searches + reads them like any other workspace
(S6.2). See [test-workspaces.md](test-workspaces.md) and
[tests/manual/non-code-workspaces.md](tests/manual/non-code-workspaces.md)
for the full walkthrough.

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
- **Git** (for vcpkg and CMake `FetchContent` of Tree-sitter grammars and PDFium)
- **vcpkg** -- typically at `C:/vcpkg`. If you don't have it yet:
  ```
  git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
  C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
  ```
  Substitute your own path if you install elsewhere, and adjust
  `-DCMAKE_TOOLCHAIN_FILE` in the configure command below accordingly.

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
English-only and weaker on long passages. The small embedder is also
lower-resolution (384-D, 33M params vs bge-m3's 1024-D, 568M params), so on
large or semantically diverse workspaces it loses recall first -- pick the
small profile if you're disk-constrained, English-only, and your queries lean
lexical; otherwise the recommended profile holds up better as the workspace
grows.

**Concrete example of the small-profile paraphrase ceiling** (from the
S5.N WS2 acceptance run on Simple English Wikipedia, 5000 articles):

- Query *"Which scientist explained how species change over time?"* on the
  small profile -> ranks `Earth.html` first, doesn't surface `Charles_Darwin.html`
  at all. The synonymy bridge between "species change over time" and
  "evolution by natural selection" (the article's actual phrasing) is too
  wide for bge-small at 384 dimensions.
- Same workspace, same query, on the recommended profile (bge-m3) -> ranks
  `Charles_Darwin.html` first.
- Sanity check: small profile ranks Darwin first when the query uses
  matching vocabulary -- *"evolution natural selection"* or *"Darwin"*
  both put `Charles_Darwin.html` at rank 1.

The takeaway: small profile works well when user queries lean lexical
(they share words with the document); it falls down when the query
paraphrases the topic in unrelated vocabulary. For agent workflows the
LLM tends to retry with closer wording when the first search misses, so
the practical hit is smaller than the worst case suggests -- but it's
real, and tied to the embedder dimensionality, not to Locus.

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
| `/compact [instructions]` | Reduce context via the layered compaction pipeline |
| `/history` | Show conversation summary and token count |
| `/breakdown` | Show the per-message context breakdown + token count |
| `/save` | Save current session |
| `/sessions` | List saved sessions |
| `/load <id>` | Resume a saved session |
| `/undo [turn_id]` | Revert files mutated by the most recent turn (0 = most recent) |
| `/metrics` | Show per-turn timing + per-tool histogram |
| `/export_metrics [json\|csv]` | Write metrics under `.locus/metrics/` |
| `/memorize [+tag] [--pin] <text>` | Add a workspace memory-bank entry inline |
| `/forget <id> [--hard]` | Delete a memory-bank entry |
| `/reload` | Re-scan prompt templates from disk |

Tool-name slashes (`/read_file`, `/search_text`, ...) run a tool directly, and any
project or global prompt template registers as its own slash command. Anything else
you type is sent as a message to the agent. The agent can call tools (read files,
search, run commands, etc.) -- each tool call is shown to you for approval (subject
to the workspace's tool-approval preset) before execution.

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

**"LLM stream stalled after retry (no data for 1800s)"** -- a local LLM took
longer than 1800 s (30 min) to send the next chunk. Common with large models
on a fresh prompt where prefill alone takes many minutes, or on tool-call-heavy
rounds where the model buffers a long string payload. Raise the watchdog
further in `<workspace>/.locus/config.json`:

```json
"llm": {
  "timeout_ms": 3600000
}
```

(Default is 1800000 ms = 30 min. Was 600000 in earlier builds.)

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
