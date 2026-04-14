# Architecture Overview

> Status: Architecture phase. Core decisions made. CLI prototype is next milestone.
> Language: C++20. Core is a headless daemon; frontends attach via API.

---

## Deployment Model

Locus Core runs as a **system tray background process** — always on, no visible window.
Frontends connect via one of two paths:

- **C++ direct** — in-process virtual interface, zero overhead. For the wxWidgets frontend only.
- **WebSocket + HTTP** — network API, language-agnostic. For all other frontends.

Both paths are symmetric: the same `IFrontend` / `ILocusCore` concepts apply to both.
The WebSocket server is an adapter that implements `IFrontend` over the wire.

```
┌──────────────────┐     ┌───────────────────────────────────────────────┐
│  wxWidgets       │     │        Tauri/Web · Mobile · Browser           │
│  (C++, v1)       │     │            (any language, any device)          │
│                  │     │                                                │
│  implements      │     │  connect via WebSocket + HTTP REST             │
│  IFrontend       │     │  local or LAN (token auth for remote)          │
└────────┬─────────┘     └──────────────────────┬────────────────────────┘
         │                                       │
         │  C++ virtual calls                    │  WebSocket + HTTP
         │  (same process, zero overhead)        │  (Crow server, JSON)
         │                                       │
┌────────▼───────────────────────────────────────▼────────────────────────┐
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
│  ┌───────────────────────────▼──────────────────────────────────────┐   │
│  │                        Agent Core                                 │   │
│  │  - Conversation manager (history, context, compaction)           │   │
│  │  - Tool dispatcher + approval gate                               │   │
│  │  - System prompt builder (LOCUS.md + workspace context)          │   │
│  └──────┬───────────────┬──────────────┬─────────────────────────── ┘   │
│         │               │              │                                 │
│  ┌──────▼──────┐  ┌─────▼──────┐  ┌───▼─────────────────────────────┐  │
│  │ LLM Client  │  │  Tool Set  │  │      Workspace Engine            │  │
│  │  LM Studio  │  │ (ITool API)│  │  File Watcher (ReadDirChangesW)  │  │
│  │  OpenAI-    │  │ - Files    │  │  FTS5 Index     (SQLite)         │  │
│  │  compat.    │  │ - Search   │  │  Vector Index   (sqlite-vec)     │  │
│  │  REST API   │  │ - Terminal │  │  ONNX Embeddings                 │  │
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
├── src/                    All source — headers and implementations live together
│   ├── main.cpp            CLI entry point
│   ├── workspace.{h,cpp}   Workspace class — owns DB, watcher, indexer, query
│   ├── database.{h,cpp}    SQLite RAII wrapper, schema creation
│   ├── file_watcher.{h,cpp} efsw wrapper, FileEvent queue with debounce
│   ├── indexer.{h,cpp}     Full/incremental indexing: FTS5, symbols, headings
│   ├── index_query.{h,cpp} Read-only query API over the index
│   ├── llm_client.{h,cpp}  ILLMClient interface + LMStudioClient (SSE streaming)
│   ├── sse_parser.{h,cpp}  Incremental SSE line parser
│   └── glob_match.h        Glob pattern matching for exclude filters
├── tests/                  Catch2 test files (one per subsystem)
├── tools/                  (reserved for future tool implementations)
├── build/                  CMake build output (git-ignored)
├── CMakeLists.txt          Root build — locus_core lib, locus exe, tree-sitter grammars
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
    └── locus.log           Rotating log file (max 10 MB × 3)
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

#### WebSocket + HTTP (all other frontends)

The `WebSocketAdapter` implements `IFrontend` and registers with Core like any other frontend.
It translates Core callbacks into JSON WebSocket messages and routes incoming messages
to `ILocusCore` calls. Remote frontends (Tauri, mobile, browser) never know they are
talking through an adapter — they see the same logical interface over the wire.

**WebSocket** — streaming and bidirectional:
- LLM token streaming, tool approval flow, index progress, compaction events

**HTTP REST** — stateless operations:
- Workspace open/list/config, session management, settings

**Authentication**:
- `127.0.0.1` connections: no auth
- LAN/remote: bearer token shown in tray settings, copied to remote client
- HTTPS: self-signed cert, trust-on-first-use

### 2. Workspace Engine
The heart of the system. Responsible for:
- Watching the workspace folder for changes (`ReadDirectoryChangesW` on Windows)
- FTS5 keyword index (SQLite + FTS5)
- Vector semantic index (sqlite-vec + ONNX in-process embeddings)
- ZIM archive support via libzim (for Kiwix/Wikipedia workspaces)
- Document text extraction (PDF via pdfium, DOCX via ZIP+XML)
- Managing the `.locus/` directory inside each workspace

See: [workspace-index.md](workspace-index.md)

### 3. Agent Core
Orchestrates the conversation loop:
- Builds the prompt: `[system base] + [LOCUS.md] + [workspace metadata] + [history] + [user message]`
- Sends to LLM, streams response back to connected frontends via API Server
- Detects tool calls in the response
- Pauses at each tool call → sends `tool_call_pending` to frontends via WebSocket
- Waits for approval from any connected frontend
- Executes approved tools → injects result → resumes LLM generation
- Manages context window (compaction, pinning, summarization)

### 4. Tool Set (ITool interface)
See: [tool-protocol.md](tool-protocol.md)

Initial tools: `read_file`, `write_file`, `create_file`, `delete_file`,
`list_directory`, `search_hybrid`, `search_text`, `search_symbols`,
`get_file_outline`, `run_command`, `web_search`, `web_fetch`, `web_read`

### 5. LLM Client
- Currently: LM Studio OpenAI-compatible REST API (localhost:1234)
- SSE streaming, tool call parsing, context length enforcement
- Abstracted behind an interface — swap backends without touching Agent Core

---

## Frontend Contract

A frontend has exactly two responsibilities regardless of connection type:
1. **Display** — render what Core sends (tokens, tool calls, index status)
2. **Input** — send what the user does (messages, approvals, edit context)

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
| wxWidgets (C++) | `IFrontend` / `ILocusCore` direct | Zero — same process, virtual calls |
| Tauri/web | WebSocket + HTTP (via `WebSocketAdapter`) | Minimal — local loopback |
| Mobile app | WebSocket + HTTP over LAN | Network RTT only |
| Browser | WebSocket + HTTP over LAN | Network RTT only |

All frontends, regardless of connection type, see the same events and share the same session.
A Rust+Tauri window and a wxWidgets window can be open simultaneously on the same session.

---

## WebSocket Message Protocol (sketch)

```
// Frontend → Core
{ "type": "send_message",      "session_id": "...", "content": "...", "edit_context": {...} }
{ "type": "tool_decision",     "call_id": "...",    "decision": "approve"|"reject"|"modify", "args": {...} }
{ "type": "set_edit_context",  "file": "...",       "line": 42, "col": 0, "selection": "..." }
{ "type": "compact_context",   "session_id": "...", "strategy": "A"|"B"|"C"|"D" }

// Core → Frontend
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
           ▼  (WebSocket: send_message)
     API Server → Agent Core
           │
           ▼
    Build prompt: LOCUS.md + workspace ctx + history + message
           │
           ▼
      LLM Client streams response
           │
           ├── text token → API Server → all frontends  (type: token)
           │
           └── tool call detected
                      │
                      ▼  (WebSocket: tool_call_pending)
               All frontends show approval dialog
                      │
                      ▼  (WebSocket: tool_decision from any frontend)
               Tool executes
                      │
                      ▼  (WebSocket: tool_result)
               Result injected into context
                      │
                      ▼
               LLM generation resumes
```

---

## Context Management Strategy

### Preventive (keep the window lean)
1. **Never pre-load** files — tools fetch only what's needed
2. **Paginate** file reads — agent requests chunks, not full files
3. **Summarize** tool results before injecting — digest + offer full on demand
4. **Workspace context injection** — only relevant metadata, not everything
5. **User-controlled pinning** — pinned items survive all compaction

### Compaction (user-controlled, never silent)
At configurable threshold (default 80%) a compaction event is sent to all frontends.
User picks a strategy:

| Strategy | How | LLM needed |
|---|---|---|
| **A — LLM Summary** | LLM condenses history to a digest (user reviews) | Yes |
| **B — Drop tool results** | Strip verbose tool output, keep turns | No |
| **C — Drop oldest turns** | User selects N turns, sees preview | No |
| **D — Save & restart** | Save session to `.locus/sessions/`, fresh context | Optional |

---

## Build Sequence

1. **CLI prototype (C++20)** — no UI, no API server. Agent core + index + LLM client.
   Validates architecture before any UI work. Tool approval via y/n prompts.
2. **API Server + wxWidgets frontend (C++20)** — add tray daemon + WebSocket server + first real UI.
3. **VS Code shim (v1.5)** — ~200 lines TypeScript, connects to Core API, sends edit context.
4. **Additional frontends (future)** — Tauri/web, mobile app — all speak the same API.

---

## Open Questions

- Remote access: LAN only vs internet tunneling (Tailscale/ngrok)? LAN first.
- Mobile: native app vs PWA (Progressive Web App)? Likely PWA first — reuses web frontend.
