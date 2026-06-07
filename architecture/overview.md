# Architecture Overview

> Status: M5 implementation. The CLI, native Windows GUI, workspace indexing,
> local-agent loop, tool approvals, MCP, retrieval, memory, metrics, and UI
> automation exist today. Connected frontends remain M6 work.
> Language: C++20. Today the CLI and wxWidgets GUI host the core in-process.
> M6 adds a WebSocket/HTTP adapter for browser, VS Code, and LAN clients.

---

## Deployment Model

Locus currently ships two entry points from the same C++ core:

- `locus.exe` -- terminal CLI for direct workspace sessions.
- `locus_gui.exe` -- native Windows desktop app using wxWidgets and WebView2.

The local machine owns the workspace work: file indexing, embeddings, reranking,
tool execution, session storage, checkpoints, logs, and memory. LLM inference is
local when the app points at LM Studio, Ollama, llama-server, or another
OpenAI-compatible local endpoint. Remote model servers are opt-in.

Frontend paths:

- **C++ direct** -- implemented. The CLI and wxWidgets GUI call the core
  in-process.
- **WebSocket + HTTP** -- planned for M6. Browser, VS Code, and LAN clients
  connect through an adapter rather than duplicating the agent runtime.

Both paths use the same `IFrontend` / `ILocusCore` concepts. The planned
WebSocket server is an adapter that implements `IFrontend` over the wire.

```
Implemented today:

  CLI / wxWidgets GUI
          |
          | C++ virtual calls
          v
  Locus Core
    - FrontendRegistry
    - AgentCore
    - LLM client
    - ToolDispatcher
    - Workspace engine
        - file watcher
        - SQLite FTS5 index
        - optional sqlite-vec vector index
        - llama.cpp embeddings
        - document extractors

Planned in M6:

  Browser / VS Code / LAN clients
          |
          | WebSocket + HTTP
          v
  Crow adapter
          |
          | same IFrontend / ILocusCore contract
          v
  Locus Core
```

---

## Project Structure

```
locus/
|-- architecture/           Design docs, ADRs, subsystem notes
|-- assets/                 Icons and generated image assets
|-- cmake/triplets/         vcpkg custom triplet
|-- models/                 Optional local embedding / reranker models
|-- queries/                Tree-sitter query snippets
|-- roadmap/                Milestone and stage notes
|-- scripts/                Repo hygiene helpers
|-- src/
|   |-- agent/              Conversation loop, context, compaction, metrics
|   |-- core/               Workspace, config, database, frontend contracts
|   |-- extractors/         Text extraction for documents and structured files
|   |-- frontends/          CLI and wxWidgets GUI
|   |-- index/              FTS, symbols, embeddings, reranking
|   |-- llm/                OpenAI-compatible client and stream decoders
|   |-- mcp/                MCP config, client, stdio transport, tool bridge
|   `-- tools/              Built-in agent tools
|-- tests/                  Unit, integration, retrieval, and UI automation tests
|-- third_party/            Vendored sqlite-vec source
|-- CMakeLists.txt
|-- CMakePresets.json
|-- vcpkg.json
`-- README.md
```

Runtime directory created inside each workspace:

```
<workspace>/
`-- .locus/                 Workspace-local data
    |-- config.json         Workspace settings
    |-- index.db            SQLite database (FTS5 + symbols + headings + files)
    |-- vectors.db          Optional vector database
    |-- sessions/           Saved conversations
    |-- checkpoints/        Undo snapshots
    `-- locus.log           Rotating log file
```

---

## Key Subsystems

### 1. Frontend Interface Layer

The core treats a frontend as a display/input surface. The frontend renders
tokens, tool calls, index status, context meters, and approval prompts; it sends
user messages and approval decisions back to the core.

Current direct frontends:

- **CLI** -- terminal REPL and y/n approval flow.
- **wxWidgets GUI** -- desktop chat surface, settings, tray, activity views,
  terminal panel, diff rendering, and UI automation hooks.

Planned M6 adapter:

- **Crow/WebSocket adapter** -- implements the same frontend contract over
  JSON messages for browser, VS Code, and LAN clients.

### 2. Workspace Engine

The workspace engine is responsible for:

- Watching the workspace folder for changes.
- Maintaining the FTS5 keyword index over files, headings, symbols, and text.
- Maintaining the optional sqlite-vec semantic index.
- Running optional llama.cpp embeddings and cross-encoder reranking.
- Extracting text from Markdown, text, JSON/YAML, HTML, PDF, DOCX, and XLSX.
- Managing `.locus/` state: config, indexes, sessions, checkpoints, metrics,
  memory, and logs.

See [workspace-index.md](workspace-index.md).

### 3. Agent Core

`AgentCore` orchestrates a conversation turn:

- Owns `LLMContext`: history, system prompt assembly, context budget, attached
  context, checkpoints, and file-change tracking.
- Sends prompts to the configured LLM client and streams tokens to frontends.
- Detects tool calls in OpenAI-style and XML-style stream formats.
- Pauses for approval, executes through `ToolDispatcher`, and appends results.
- Records activity and metrics, handles slash commands, and manages compaction.

### 4. Tool Set

Built-in tools include file operations, directory listing, unified search
(`text`, `symbols`, `semantic`, `hybrid`, `regex`, `ast`), file outlines,
foreground and background commands, process inspection/output/stop, plan-mode
tools, memory tools, and interactive `ask_user`. MCP can add external tools at
runtime. Web retrieval tools are planned for M6.

See [tool-protocol.md](tool-protocol.md).

### 5. LLM Client

- OpenAI-compatible REST/SSE client for local servers such as LM Studio.
- Model/context auto-detection where the server exposes metadata.
- Stream decoders for OpenAI tool calls plus XML-style formats used by local
  models.
- Context length enforcement, stall timeout handling, sampler options, and
  model presets.
- Abstracted behind `ILLMClient` so backend swaps do not touch Agent Core.

---

## Frontend Contract

A frontend has two responsibilities:

1. **Display** -- render what Core sends.
2. **Input** -- send what the user does.

Core handles the agent logic, tools, workspace state, prompt assembly, and
conversation history.

```
Frontend responsibilities:          Core responsibilities:
  - Render streaming text             - Run the LLM
  - Show tool approval dialog         - Execute tools
  - Show context meter                - Manage conversation history
  - Send user messages                - Build system prompts
  - Send edit context                 - Maintain workspace index
  - Approve/reject tool calls         - Handle compaction
```

| Frontend | Connection | Status |
|---|---|---|
| CLI (C++) | `IFrontend` / `ILocusCore` direct | Implemented |
| wxWidgets (C++) | `IFrontend` / `ILocusCore` direct | Implemented |
| Crow adapter (C++) | `IFrontend` / `ILocusCore` direct | Planned M6 |
| Browser client | WebSocket + HTTP via Crow adapter | Planned M6 |
| VS Code extension | WebSocket + HTTP via Crow adapter | Planned M6 |
| Mobile/PWA client | WebSocket + HTTP via Crow adapter | Planned |

---

## WebSocket Message Protocol (Planned Sketch)

```json
// Frontend -> Core
{ "type": "send_message", "session_id": "...", "content": "...", "edit_context": {} }
{ "type": "tool_decision", "call_id": "...", "decision": "approve", "args": {} }
{ "type": "set_edit_context", "file": "...", "line": 42, "col": 0, "selection": "..." }
{ "type": "compact_context", "session_id": "...", "strategy": "A" }

// Core -> Frontend
{ "type": "token", "content": "..." }
{ "type": "tool_call_pending", "call_id": "...", "tool": "...", "args": {}, "preview": "..." }
{ "type": "tool_result", "call_id": "...", "display": "..." }
{ "type": "message_complete" }
{ "type": "context_meter", "used": 4200, "limit": 8192 }
{ "type": "index_progress", "workspace": "...", "fts": [1200, 5000], "vec": [300, 5000] }
{ "type": "compaction_needed", "session_id": "...", "used_pct": 82 }
```

---

## Data Flow: A Single Turn

```
User types message in frontend
           |
           v
     Frontend -> Agent Core
           |
           v
    Build prompt: LOCUS.md + workspace context + history + message
           |
           v
      LLM client streams response
           |
           |-- text token -> frontend
           |
           `-- tool call detected
                      |
                      v
               Frontend shows approval dialog
                      |
                      v
               Tool executes
                      |
                      v
               Result injected into context
                      |
                      v
               LLM generation resumes
```

---

## Context Management Strategy

### Preventive

1. **Never pre-load files** -- tools fetch only what is needed.
2. **Paginate file reads** -- the agent requests chunks, not whole files.
3. **Keep tool manifests lean** -- capabilities hide irrelevant tools.
4. **Preserve prompt stability** -- volatile per-turn context lives outside the
   stable system prompt when possible.
5. **Enforce reserve headroom** -- the agent refuses a generation when the
   prompt would consume the response reserve.

### Compaction

At configurable thresholds, Locus can compact history using deterministic layers
and, optionally, an LLM-generated summary. Recent turns and compact user/tool
context are preserved by the default passes.

---

## Trust Boundary

Locus draws its trust boundary by **authorship and curation, not by file location**.

- **Trusted: workspace content.** The files in the opened folder are content the user
  wrote or deliberately curated (their code, docs, cloned repos). They are read and acted
  on as-is and are never run through the prompt-injection scanner. The user vouches for the
  workspace by choosing to open it; the precondition is "only point Locus at folders you trust."
- **Untrusted: external ingress.** Any text authored by a third party that the agent surfaces
  without the user having reviewed it -- web pages (S6.1), offline knowledge bases such as
  Wikipedia / ZIM (S6.2), and MCP tool output (S4.G) -- crosses the boundary on the way in.
  The discriminator is "did the user author or review this specific text," which is why a
  Wikipedia / ZIM dump is untrusted even though the file physically sits in the workspace: the
  user chose the *source*, not the *content* of six million public-authored articles.

External ingress is scanned and origin-stamped on entry (S6.0); the taint travels into the
index so the untrusted framing re-surfaces at the search hit, not just at fetch time. The
scanner is a visibility tripwire, not an enforcement boundary -- the hard control remains the
approval gate, which every mutating / egress tool call passes regardless of where its inputs
came from.

---

## Build Sequence

1. **CLI prototype (C++20)** -- implemented.
2. **wxWidgets frontend (C++20)** -- implemented.
3. **Crow adapter (C++20)** -- planned for M6.
4. **External clients** -- browser, VS Code, and mobile/PWA clients connect
   through the Crow adapter once it exists.

---

## See Also

- [agent-loop.md](agent-loop.md) -- turn orchestration, threading, `AgentCore` split
- [tool-protocol.md](tool-protocol.md) -- `ITool` contract and approval policies
- [workspace-index.md](workspace-index.md) -- index schema and retrieval design
- [tech-stack.md](tech-stack.md) -- library choices and rationale
- [web-retrieval.md](web-retrieval.md) -- planned web RAG pipeline
- [decisions/](decisions/) -- ADR trail for load-bearing architectural decisions
