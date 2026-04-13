# Requirements

## Core Concepts

### Workspace
Every app instance is bound to exactly one **workspace** — a local folder on disk.
The workspace is selected at startup and does not change during a session.
A workspace config file is created/updated in the folder describing:
- Access level: read-only or read-write
- Purpose/type: code project, document library, knowledge base, notebook, etc.
- Index database location and last-updated timestamp
- Agent behavior settings specific to this workspace

### Session
A conversation session between the user and the agent, within the context of a workspace.
Sessions have a history but context management must keep the active window lean.

### Active Edit Context
The user's current "point of attention" in a file: which file is open, what text is selected,
what line/character the cursor is at. The agent can see and reference this context directly —
like a colleague looking at the same screen. The user can toggle it on/off per message.
This is distinct from the workspace index: the index is the map, edit context is the spotlight.

---

## Functional Requirements

### F1 — Workspace Management
- [ ] Open/bind to a local folder as workspace
- [ ] Create/update `.locus/config.json` in workspace root
- [ ] Store workspace metadata: access mode, type, description, index info
- [ ] Support read-only mode (agent can search but not modify files)
- [ ] Support read-write mode (agent can create/edit/delete files)
- [ ] On first open, offer to create a `LOCUS.md` template in the workspace root

### F1b — Workspace Instructions (LOCUS.md)
A plain markdown file in the workspace root that the user writes and maintains.
Injected verbatim into the system prompt on **every** LLM request for this workspace.
This is the user's persistent context layer — workspace purpose, conventions, constraints.

- [ ] If `LOCUS.md` exists in workspace root, read it and prepend to system prompt
- [ ] LOCUS.md is exempt from all compaction — never trimmed, never summarized
- [ ] Token count of LOCUS.md shown in UI (so user knows the per-request overhead)
- [ ] If LOCUS.md exceeds a configurable token limit, warn the user (default: 500 tokens)
- [ ] Locus never modifies LOCUS.md automatically — it is always user-authored
- [ ] Changes to LOCUS.md on disk take effect on the next LLM request (no restart needed)

Example LOCUS.md contents:
```
This is a Vulkan renderer codebase (C++20, GLSL 4.6, Windows/Linux).
Entry point: src/main.cpp. Shaders: assets/shaders/.
Never suggest OpenGL. Prefer explicit synchronization over implicit.
Build: CMake + vcpkg, target MSVC 2022.
```

### F2 — File Operations (Agent Tools)
- [ ] Read file contents (full or partial/paginated for large files)
- [ ] Write/overwrite file
- [ ] Create new file (with path)
- [ ] Delete file (with user confirmation prompt)
- [ ] List directory contents
- [ ] Move/rename file
- [ ] Diff two files or file versions

### F3 — Workspace Index
- [ ] Build a hierarchical index of all files in the workspace on first open
- [ ] Index stores: file path, size, last-modified, type/extension, a brief summary or extracted metadata
- [ ] For text files: extract headings, class/function names, key terms (non-LLM, algorithmic)
- [ ] For code files: parse structure (classes, functions, imports) using language-aware parsers
- [ ] Auto-update index when files change (watcher — not LLM-driven)
- [ ] Expose the index as a fast query tool for the agent: "find files matching X", "find functions named Y"
- [ ] Index must support large workspaces (100k+ files, multi-GB text corpora)

### F4 — Search
- [ ] **Keyword search** — FTS5 full-text search with BM25 ranking (always available)
- [ ] **Symbol search** — find classes/functions/methods by name via Tree-sitter index
- [ ] **Semantic search** — vector similarity via ONNX in-process embedding model
      - Opt-in per workspace (disabled by default — costs CPU + disk for embeddings)
      - Default model: `all-MiniLM-L6-v2` (22MB, 384-dim, runs on CPU)
      - Upgrade option: `nomic-embed-text-v1.5` (280MB, higher quality)
- [ ] **Hybrid search** — BM25 + semantic merged via Reciprocal Rank Fusion (default tool)
- [ ] Regex search over file contents
- [ ] File-type, path-pattern, and language filtering on all search types
- [ ] All results returned as ranked snippets with file path + line range — never full files
- [ ] Progress indicator while embedding index is being built in background

### F5 — Terminal / Command Execution
- [ ] Agent can propose a terminal command; user approves before execution
- [ ] Execute command in workspace directory
- [ ] Stream stdout/stderr output back to UI
- [ ] Support long-running processes (build, server start)
- [ ] Kill/interrupt a running process

### F6 — Conversation / Agent Interface
- [ ] Chat interface: user sends message, agent responds
- [ ] Agent has access to a defined set of tools (F2, F3, F4, F5, F9)
- [ ] Tool calls are visible to the user in the UI before and after execution
- [ ] User can approve, modify, or reject any tool call
- [ ] Agent maintains a compact working context (not the entire file system)
- [ ] User can pin specific files or snippets into context (pinned items survive compaction)
- [ ] Context window usage indicator visible at all times — token count + % used

### F6b — Context Compaction (User-Controlled)
When the context window approaches its limit, the system **must not silently drop content**.
The user is presented with a compaction dialog and chooses the strategy.

- [ ] Threshold warning: notify user when context reaches a configurable % of limit (e.g. 80%)
- [ ] Hard stop before sending if context would overflow — never silently truncate
- [ ] Compaction dialog presents the following options (user picks one):

  **Option A — LLM Summary (smart)**
  Use the LLM to summarize the conversation so far into a compact digest.
  User can review the proposed summary before it replaces the history.
  Pinned items are excluded from summarization and kept verbatim.

  **Option B — Drop tool results**
  Keep all conversation turns (user + assistant messages) but strip out
  verbose tool call results (file contents, search results, command output).
  These are the biggest context consumers and often not needed for reasoning continuity.

  **Option C — Drop oldest turns**
  Remove the N oldest conversation turns (user selects N, sees a preview of what will be dropped).
  Simple, predictable, no LLM call needed.

  **Option D — Save and restart**
  Save the full current conversation to `.locus/sessions/<timestamp>.md`.
  Start a fresh context, optionally with a user-written or LLM-generated briefing
  that the user can edit before sending as the first message of the new session.

- [ ] Compaction is always visible: user sees before/after token counts
- [ ] User can trigger manual compaction at any time (not just when limit approached)
- [ ] Per-workspace default strategy can be configured (used as the pre-selected option in dialog)

### F7 — Active Edit Context
- [ ] Agent is aware of which file the user currently has open / focused
- [ ] Agent can see the current cursor position (file path + line + character)
- [ ] Agent can see the current text selection (start/end positions + selected text)
- [ ] User can include/exclude this context per message (toggle)
- [ ] Context is shown to the user as a visible "attached" snippet before sending
- [ ] Works across deployment modes:
  - VS Code extension: via VS Code editor API (free)
  - VS Code + standalone hybrid: extension sends context over IPC
  - Standalone only: user explicitly attaches a file region via UI gesture

### F9 — Web Search (Optional Tool)
- [ ] Agent can request a web search
- [ ] User approves before the search executes
- [ ] Results returned as summarized snippets, not raw HTML dumps
- [ ] Can be disabled per workspace (offline-only mode)

### F10 — LLM Backend
- [ ] Connect to local LLM via LM Studio API (OpenAI-compatible REST)
- [ ] Configurable: model name, endpoint URL, temperature, context length
- [ ] Display active model and context usage in UI
- [ ] Graceful handling of model not available / endpoint unreachable
- [ ] Designed for future: swap in Claude/OpenAI API as alternative backend

### F11 — System Prompt / Agent Persona
- [ ] Default system prompt optimized for token efficiency and tool use
- [ ] Per-workspace system prompt override
- [ ] System prompt includes workspace context: type, access mode, available tools

---

## Non-Functional Requirements

### Performance
- Index queries must return in < 100ms for typical workspaces
- File watcher must update index incrementally, not rebuild from scratch
- UI must remain responsive during LLM inference (streaming responses)

### Reliability
- Index must be consistent with filesystem at all times
- No data loss: never overwrite files without either user approval or explicit write-mode workspace

### Transparency
- Every agent action is visible: tool name, arguments, result summary
- User can expand any tool result to see full output
- Token usage tracked and displayed

### Extensibility
- Architecture must support: VS Code extension, CLI mode, web UI
- Tool system must be pluggable (add new tools without core changes)
- LLM backend must be swappable

---

## Nice-to-Have (Later)

- [ ] Multiple workspaces open simultaneously (tabbed)
- [ ] Session history persistence and search
- [ ] Workspace templates (code project, wiki, notebook)
- [ ] Lightweight embedding model for semantic search (runs locally)
- [ ] Export session as markdown report
- [ ] Plugin system for community tools
- [ ] Automated coding pipeline (plan → code → test → fix loop), user-supervised
- [ ] Online model backends (Claude, OpenAI, Gemini)
- [ ] VS Code extension host
- [ ] Diff viewer for AI-proposed file changes before accepting
  <!-- impl note: v1 = line-level diff rendered in ImGui (dtl or similar C++ lib).
       When VS Code shim is active: "open in VS Code diff" button calls `code --diff old new` for free. -->
