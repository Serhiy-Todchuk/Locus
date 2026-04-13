# Architecture Overview

> Status: Architecture phase. Core decisions made. CLI prototype is next milestone.
> Language: C++20. Deployment: standalone app. VS Code editor context: thin shim at v1.5.

---

## High-Level Components

```
┌─────────────────────────────────────────────────────────────┐
│                        UI Layer                             │
│  (Desktop app / VS Code extension / CLI / Web UI)           │
│  - Chat interface                                           │
│  - Tool call approval panel                                 │
│  - Context window indicator                                 │
│  - Workspace file browser                                   │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│                    Agent Core                               │
│  - Conversation manager (message history, context trimming) │
│  - Tool dispatcher (route tool calls → tool implementations)│
│  - Approval gate (pause on tool calls, await user input)    │
│  - System prompt builder (workspace context injection)      │
└──────┬──────────────┬──────────────┬────────────────────────┘
       │              │              │
┌──────▼──────┐ ┌─────▼──────┐ ┌────▼────────────────────────┐
│  LLM Client │ │  Tool Set  │ │   Workspace Engine           │
│             │ │            │ │                              │
│  LM Studio  │ │  - Files   │ │  - File Watcher              │
│  (OpenAI-   │ │  - Search  │ │  - Index Builder             │
│  compatible │ │  - Terminal│ │  - Index Query API           │
│  REST API)  │ │  - Web     │ │  - Config Manager            │
│             │ │  - Index   │ │                              │
└─────────────┘ └────────────┘ └──────────────────────────────┘
```

---

## Key Subsystems

### 1. Workspace Engine
The heart of the system. Responsible for:
- Watching the workspace folder for changes
- Maintaining the index database (updated algorithmically, never by LLM)
- Exposing a fast query API that the agent's tools call
- Managing the `.locus/` directory inside each workspace

See: [workspace-index.md](workspace-index.md)

### 2. Agent Core
Orchestrates the conversation loop:
- Builds the prompt: `[system base] + [LOCUS.md] + [workspace metadata] + [history] + [user message]`
- Sends to LLM, receives streaming response
- Detects tool calls in the response
- Pauses at each tool call → presents to user for approval
- Executes approved tool calls → injects results → continues
- Manages context window (trims old messages, summarizes if needed)

### 3. Tool Set
Each tool is a discrete, testable unit with:
- Name and description (fed to LLM in system prompt)
- Input schema (JSON Schema)
- Implementation (actual code)
- User-facing display (what the user sees in the approval panel)

Initial tools:
- `read_file(path, offset?, length?)` — paginated file reading
- `write_file(path, content)` — write/overwrite a file
- `create_file(path, content)` — create new file
- `delete_file(path)` — delete (requires explicit user approval)
- `list_directory(path, depth?)` — list dir tree
- `search_text(query, path?, filetype?)` — full-text search via index
- `search_code(query, language?)` — code-aware search via index
- `query_index(query)` — natural-language query over index metadata
- `run_command(command, cwd?)` — terminal execution
- `web_search(query)` — optional, can be disabled

### 4. LLM Client
Thin abstraction over the LLM backend:
- Currently: LM Studio OpenAI-compatible REST API
- Streaming support
- Context length enforcement
- Retry / error handling
- Future: swap in other backends

### 5. UI Layer
First target: desktop app (Tauri or native C++)
- Chat panel (streaming output)
- Tool approval panel (shows pending tool call, approve/modify/reject)
- Context meter (tokens used / available)
- Workspace panel (file tree, index status)
- Settings (model endpoint, workspace config)

---

## Data Flow: A Single Turn

```
User types message
       │
       ▼
Agent Core builds prompt
  [system prompt + workspace context + history + user message]
       │
       ▼
LLM Client streams response
       │
       ├─── Text chunk → stream to UI
       │
       └─── Tool call detected
                  │
                  ▼
            Approval Gate
            User sees: tool name + args
            User: Approve / Modify / Reject
                  │
                  ▼ (approved)
            Tool Implementation runs
                  │
                  ▼
            Result injected into context
                  │
                  ▼
            Continue LLM generation
```

---

## Context Management Strategy

This is critical for local LLMs with limited context windows (e.g. 8k–32k tokens).

### Preventive principles (keep the window lean)
1. **Never pre-load** files into context. Use tools to fetch only what's needed.
2. **Paginate** large file reads — agent requests chunks, not full files.
3. **Summarize** tool results before injecting — return a digest + offer full result on demand.
4. **Workspace context injection** — only inject relevant workspace metadata, not everything.
5. **User-controlled pinning** — user pins specific files/snippets that survive any compaction.

### When the window fills up — compaction (user-controlled)
**Rule: never silently truncate.** The user decides what gets dropped or summarized.

At a configurable threshold (default 80% full), or on user request, a compaction dialog
presents four strategies. The user picks one, reviews the result, then approves:

| Strategy | How | LLM needed | Notes |
|---|---|---|---|
| **A — LLM Summary** | LLM condenses history into a digest | Yes | User reviews before applying |
| **B — Drop tool results** | Strip verbose tool output, keep conversation turns | No | Biggest wins, minimal information loss |
| **C — Drop oldest turns** | User selects N turns to remove, sees preview | No | Predictable, surgical |
| **D — Save & restart** | Save full session to `.locus/sessions/`, fresh context | Optional | LLM can draft the briefing for new session |

Pinned items are always excluded from compaction and kept verbatim.
Manual compaction can be triggered by the user at any time.
Per-workspace config stores the default pre-selected strategy.

---

## Workspace Index Design

See: [workspace-index.md](workspace-index.md)

---

## Build Sequence (Decided)

1. **CLI prototype (C++20)** — No UI. Agent core + index + LLM client. Proves the hard parts.
2. **Desktop app (C++20)** — Full standalone app. UI framework TBD after prototype (ImGui likely).
3. **VS Code shim (v1.5)** — ~200 lines TypeScript. Sends active file/cursor/selection to running app via local socket.
4. **VS Code extension (future)** — Full embedded UI inside VS Code, if desired later.

---

## Open Questions

- Desktop UI framework: Dear ImGui + DX11 vs Sciter vs WinUI3 — deferred until CLI prototype done
- Embedding model for semantic search: deferred to v2
- VS Code shim protocol: local TCP socket vs named pipe vs HTTP — decide at v1.5
