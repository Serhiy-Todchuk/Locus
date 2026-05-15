# Architecture Overview

> Status: M5 implementation. CLI, native Windows GUI, workspace indexing,
> local-agent loop, tool approvals, MCP, retrieval, memory, metrics, and UI
> automation are implemented. M6 will add connected/front-end expansion.
> Language: C++20. Today the CLI and wxWidgets GUI host the core in-process;
> M6 introduces the WebSocket/HTTP adapter for external frontends.

---

## Deployment Model

The current product ships two entry points from the same C++ core:

- `locus.exe` -- terminal CLI for direct workspace sessions.
- `locus_gui.exe` -- native Windows desktop app using wxWidgets and WebView2.

Your PC does the local work: file indexing, embeddings, reranking, tool execution,
session storage, checkpoints, logs, and memory. LLM inference is local when pointed
at LM Studio/Ollama/llama-server-style OpenAI-compatible endpoints; remote model
servers are opt-in.

The M6 target is to split the same core behind a WebSocket/HTTP adapter so browser,
VS Code, and LAN clients can attach without duplicating the agent runtime.

Frontends connect via one of two paths:

- **C++ direct** -- implemented. The wxWidgets frontend and CLI call the core in-process.
- **WebSocket + HTTP** -- planned for M6. Language-agnostic API for external frontends.

Both paths are symmetric: the same `IFrontend` / `ILocusCore` concepts apply to both.
The WebSocket server is an adapter that implements `IFrontend` over the wire.

```
┌──────────────────┐     ┌───────────────────────────────────────────────┐
│  wxWidgets       │     │        Browser · VS Code · Mobile             │
│  (C++, v1)       │     │         (external clients, any device)         │
│                  │     │                                                │
│  implements      │     │  connect via CrowServer (WebSocket + HTTP)     │
│  IFrontend       │     │  local or LAN (token auth for remote)          │
└────────┬─────────┘     └──────────────────────┬────────────────────────┘
         │                                       │
         │  C++ virtual calls                    │  WebSocket + HTTP
         │  (same process, zero overhead)        │  (Crow server, JSON)
         │                                       │
┌────────v───────────────────────────────────────v────────────────────────┐
│                              Locus Core                                  │
│                       (C++20, system tray daemon)                        │
│                                                                          │
│   ILocusCore  ◄──── frontends call in via this interface                 │
│   IFrontend   ◄──── Core calls back on each registered frontend          │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  Frontend Registry                                                │   │
│  │  - wxWidgets frontend registered directly (IFrontend* ptr)       │   │
│  │  - WebSocket adapter registered once (fans out to N remote)      │   │
│  │  - All registered frontends receive the same events              │   │
│  └───────────────────────────┬──────────────────────────────────────┘   │
│                              │                                           │
│  ┌───────────────────────────v──────────────────────────────────────┐   │
│  │                        Agent Core                                 │   │
│  │  - Conversation manager (history, context, compaction)           │   │
│  │  - Tool dispatcher + approval gate                               │   │
│  │  - System prompt builder (LOCUS.md + workspace context)          │   │
│  └──────┬───────────────┬──────────────┬─────────────────────────── ┘   │
│         │               │              │                                 │
│  ┌──────v──────┐  ┌─────v──────┐  ┌───v─────────────────────────────┐  │
│  │ LLM Client  │  │  Tool Set  │  │      Workspace Engine            │  │
│  │  LM Studio  │  │ (ITool API)│  │  File Watcher (ReadDirChangesW)  │  │
│  │  OpenAI-    │  │ - Files    │  │  FTS5 Index     (SQLite)         │  │
│  │  compat.    │  │ - Search   │  │  Vector Index   (sqlite-vec)     │  │
│  │  REST API   │  │ - Terminal │  │  llama.cpp Embeddings (GGUF)     │  │
│  └─────────────┘  │ - Web      │  │  ZIM Reader     (libzim)         │  │
│                   └────────────┘  │  Doc Extractors (pdfium/pugixml) │  │
│                                   └─────────────────────────────────-┘  │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## Project Structure

```
locus/
├── architecture/           Design docs (this file, tech stack, tool protocol, etc.)
├── cmake/
│   └── triplets/           vcpkg custom triplet (x64-windows-static)
├── src/                    All source -- headers and implementations live together
│   ├── main.cpp            CLI entry point
│   ├── workspace.{h,cpp}   Workspace class -- owns DB, watcher, indexer, query
│   ├── database.{h,cpp}    SQLite RAII wrapper, schema creation
│   ├── file_watcher.{h,cpp} efsw wrapper, FileEvent queue with debounce
│   ├── index/              Indexing subsystem (S3.D)
│   │   ├── indexer.{h,cpp}      Full/incremental indexing: orchestrates extractors + symbols + chunks
│   │   ├── index_query.{h,cpp}  Read-only query API over the index
│   │   ├── tree_sitter_registry.{h,cpp}  TSParser + name->TSLanguage* map (shared with future ast_search)
│   │   ├── symbol_extractor.{h,cpp}      ISymbolExtractor + RuleBasedSymbolExtractor + per-language registry
│   │   ├── symbol_extractors/   Per-language SymbolRule tables (cpp, python, js_ts, go, rust, java, csharp)
│   │   ├── prepared_statements.{h,cpp}   IndexerStatements RAII holder for the 14 sqlite_stmts
│   │   ├── chunker.{h,cpp}      Code/document/sliding-window chunking
│   │   └── glob_match.h         Glob pattern matching for exclude filters
│   ├── llm_client.{h,cpp}  ILLMClient interface + LMStudioClient (SSE streaming)
│   └── sse_parser.{h,cpp}  Incremental SSE line parser
├── tests/                  Catch2 test files (one per subsystem)
├── tools/                  (reserved for future tool implementations)
├── build/                  CMake build output (git-ignored)
├── CMakeLists.txt          Root build -- locus_core lib, locus exe, tree-sitter grammars
├── CMakePresets.json        Debug/Release presets
├── vcpkg.json              Dependency manifest
├── CLAUDE.md               Claude Code working guide
├── roadmap.md              Milestones, stages, tasks
├── vision.md               Project vision and goals
├── requirements.md         Full feature list
├── DIFFERENTIATORS.md      What makes Locus different
└── test-workspaces.md      Three test workspace definitions
```

Runtime directory created inside each workspace:
```
<workspace>/
└── .locus/                 Workspace-local data (git-ignored by users)
    ├── config.json         Workspace settings (exclude patterns, toggles)
    ├── index.db            SQLite database (FTS5 + symbols + headings + files)
    └── locus.log           Rotating log file (max 10 MB x 3)
```

---

## Key Subsystems

### 1. Frontend Interface Layer

Two connection paths, one symmetric design.

#### C++ Direct Interface (wxWidgets frontend)

```cpp
// Core calls these on every registered frontend (wxWidgets implements this directly)
class IFrontend {
public:
    virtual void on_token(std::string_view token) = 0;
    virtual void on_tool_call_pending(const ToolCall& call) = 0;
    virtual void on_tool_result(const std::string& call_id,
                                const std::string& display) = 0;
    virtual void on_message_complete() = 0;
    virtual void on_context_meter(int used_tokens, int limit) = 0;
    virtual void on_index_progress(const IndexProgress& p) = 0;
    virtual void on_compaction_needed(const std::string& session_id,
                                      int used_pct) = 0;
};

// Frontends call these into the Core (wxWidgets calls these directly)
class ILocusCore {
public:
    virtual void register_frontend(IFrontend* fe) = 0;
    virtual void unregister_frontend(IFrontend* fe) = 0;

    virtual void send_message(const SendMessageParams& p) = 0;
    virtual void tool_decision(const std::string& call_id,
                               ToolDecision decision,
                               const nlohmann::json& modified_args = {}) = 0;
    virtual void set_edit_context(const EditContext& ctx) = 0;
    virtual void compact_context(const std::string& session_id,
                                 CompactionStrategy strategy) = 0;

    virtual WorkspaceInfo open_workspace(const std::string& path) = 0;
    virtual SessionInfo   create_session(const std::string& workspace_id) = 0;
    virtual IndexStatus   get_index_status(const std::string& workspace_id) = 0;
};
```

The wxWidgets frontend lives in the same process as Core. It gets an `ILocusCore*` at startup,
registers itself as an `IFrontend`, and calls/receives everything with zero overhead.
Agent thread events are marshalled to the UI thread via `wxQueueEvent()` + custom `wxThreadEvent` types.

#### WebSocket + HTTP (CrowServer -> external clients)

The `CrowFrontend` implements `IFrontend` and registers with Core like any other frontend.
It runs a Crow HTTP/WebSocket server and translates Core callbacks into JSON WebSocket
messages, routing incoming messages to `ILocusCore` calls. External clients (browser,
VS Code extension, mobile app) connect to this server -- they see the same logical
interface over the wire.

**WebSocket** -- streaming and bidirectional:
- LLM token streaming, tool approval flow, index progress, compaction events

**HTTP REST** -- stateless operations:
- Workspace open/list/config, session management, settings

**Authentication**:
- `127.0.0.1` connections: no auth
- LAN/remote: bearer token shown in tray settings, copied to remote client
- HTTPS: self-signed cert, trust-on-first-use

### 2. Workspace Engine
The heart of the system. Responsible for:
- Watching the workspace folder for changes.
- FTS5 keyword index over files, headings, symbols, and extracted text.
- Optional vector semantic index with sqlite-vec and llama.cpp GGUF embeddings.
- Optional cross-encoder reranking for top-K precision.
- Document text extraction for Markdown, text, JSON/YAML, HTML, PDF, DOCX, XLSX,
  and ZIP/XML-backed formats.
- Managing the `.locus/` directory inside each workspace: config, indexes,
  sessions, checkpoints, metrics, memory, and logs.

See: [workspace-index.md](workspace-index.md)

### 3. Agent Core
Orchestrates the conversation loop:
- Owns `LLMContext`: conversation history, immutable system prompt assembly,
  context budget, file-change tracker, attached context, checkpoints, and session ids.
- Sends prompts to the configured LLM client and streams tokens to the active frontend.
- Detects tool calls in OpenAI-style and XML-style stream formats.
- Pauses at tool calls, asks the frontend for approval or argument edits, then executes
  through `ToolDispatcher`.
- Records activity and metrics, handles slash commands, and manages compaction.

### 4. Tool Set (ITool interface)
See: [tool-protocol.md](tool-protocol.md)

Current built-in tools include file operations, directory listing, unified search
(`text`, `symbols`, `semantic`, `hybrid`, `regex`, `ast`), file outlines, foreground
and background commands, process inspection/output/stop, plan-mode tools, memory tools,
and interactive `ask_user`. MCP can add external tools at runtime. Web retrieval tools
remain planned for M6.

### 5. LLM Client
- OpenAI-compatible REST/SSE client for local servers such as LM Studio.
- Model/context auto-detection where the server exposes metadata.
- Stream decoders for OpenAI-style tool calls plus XML-style formats used by local models.
- Context length enforcement, stall timeout handling, sampler options, and model presets.
- Abstracted behind `ILLMClient` so backend swaps do not touch Agent Core.

---

## Frontend Contract

A frontend has exactly two responsibilities regardless of connection type:
1. **Display** -- render what Core sends (tokens, tool calls, index status)
2. **Input** -- send what the user does (messages, approvals, edit context)

Core handles all logic. Frontends are display + input only.

```
Frontend responsibilities:          Core responsibilities:
  - Render streaming text             - Run the LLM
  - Show tool approval dialog         - Execute tools
  - Show context meter                - Manage conversation history
  - Send user messages                - Build system prompts
  - Send edit context                 - Maintain workspace index
  - Approve/reject tool calls         - Handle compaction
```

| Frontend | Connection | Overhead |
|---|---|---|
| CLI (C++) | `IFrontend` / `ILocusCore` direct | Zero -- same process, virtual calls |
| wxWidgets (C++) | `IFrontend` / `ILocusCore` direct | Zero -- same process, virtual calls |
| CrowServer (C++) | `IFrontend` / `ILocusCore` direct | Zero -- same process, serves external clients |
| Browser client | WebSocket + HTTP via CrowServer | Minimal -- local loopback |
| VS Code extension | WebSocket + HTTP via CrowServer | Minimal -- local loopback |
| Mobile app | WebSocket + HTTP via CrowServer over LAN | Network RTT only |

All three C++ frontends (CLI, wxWidgets, CrowServer) implement `IFrontend` and register
directly with Core. External clients connect to CrowServer. Multiple frontends and clients
can be active simultaneously on the same session.

---

## WebSocket Message Protocol (sketch)

```
// Frontend -> Core
{ "type": "send_message",      "session_id": "...", "content": "...", "edit_context": {...} }
{ "type": "tool_decision",     "call_id": "...",    "decision": "approve"|"reject"|"modify", "args": {...} }
{ "type": "set_edit_context",  "file": "...",       "line": 42, "col": 0, "selection": "..." }
{ "type": "compact_context",   "session_id": "...", "strategy": "A"|"B"|"C"|"D" }

// Core -> Frontend
{ "type": "token",             "content": "..." }
{ "type": "tool_call_pending", "call_id": "...", "tool": "...", "args": {...}, "preview": "..." }
{ "type": "tool_result",       "call_id": "...", "display": "..." }
{ "type": "message_complete" }
{ "type": "context_meter",     "used": 4200, "limit": 8192 }
{ "type": "index_progress",    "workspace": "...", "fts": [1200,5000], "vec": [300,5000] }
{ "type": "compaction_needed", "session_id": "...", "used_pct": 82 }
```

---

## Data Flow: A Single Turn

```
User types message in frontend
           │
           v  (WebSocket: send_message)
     API Server -> Agent Core
           │
           v
    Build prompt: LOCUS.md + workspace ctx + history + message
           │
           v
      LLM Client streams response
           │
           ├── text token -> API Server -> all frontends  (type: token)
           │
           └── tool call detected
                      │
                      v  (WebSocket: tool_call_pending)
               All frontends show approval dialog
                      │
                      v  (WebSocket: tool_decision from any frontend)
               Tool executes
                      │
                      v  (WebSocket: tool_result)
               Result injected into context
                      │
                      v
               LLM generation resumes
```

---

## Context Management Strategy

### Preventive (keep the window lean)
1. **Never pre-load** files -- tools fetch only what's needed
2. **Paginate** file reads -- agent requests chunks, not full files
3. **Summarize** tool results before injecting -- digest + offer full on demand
4. **Workspace context injection** -- only relevant metadata, not everything
5. **User-controlled pinning** -- pinned items survive all compaction

### Compaction (user-controlled, never silent)
At configurable threshold (default 80%) a compaction event is sent to all frontends.
User picks a strategy:

| Strategy | How | LLM needed |
|---|---|---|
| **A -- LLM Summary** | LLM condenses history to a digest (user reviews) | Yes |
| **B -- Drop tool results** | Strip verbose tool output, keep turns | No |
| **C -- Drop oldest turns** | User selects N turns, sees preview | No |
| **D -- Save & restart** | Save session to `.locus/sessions/`, fresh context | Optional |

---

## Build Sequence

1. **CLI prototype (C++20)** -- no UI, no API server. Agent core + index + LLM client.
   Validates architecture before any UI work. Tool approval via y/n prompts.
2. **wxWidgets frontend (C++20)** -- system tray daemon + first real UI.
3. **CrowServer frontend (C++20)** -- HTTP/WebSocket server, enables external clients.
4. **External clients** -- VS Code extension, browser web page, mobile app -- all connect to CrowServer.

---

## Open Questions

- Remote access: LAN only vs internet tunneling (Tailscale/ngrok)? LAN first.
- Mobile: native app vs PWA (Progressive Web App)? Likely PWA first -- reuses web frontend.

---

## See also

- [agent-loop.md](agent-loop.md) -- turn orchestration, threading, `AgentCore` collaborator split, M4 hook points
- [tool-protocol.md](tool-protocol.md) -- `ITool` contract, approval policies, adding a new tool
- [workspace-index.md](workspace-index.md) -- index schema, incremental updates, FTS5 + vector query API
- [tech-stack.md](tech-stack.md) -- library choices and rationale
- [web-retrieval.md](web-retrieval.md) -- web RAG pipeline (fetch -> index -> search)
- [decisions/](decisions/) -- ADR trail for load-bearing architectural decisions
