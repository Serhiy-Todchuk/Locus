# What Makes Locus Different

> This document exists to keep the project honest as it evolves.
> When a feature decision feels unclear, come back here and ask:
> does this serve the people we're actually building for?

---

## Who This Is For

People who:
- Have private data (code, documents, notes, archives) they will not or cannot send to the cloud
- Want AI assistance that still works when the internet is down
- Are frustrated that local LLM workflows waste scarce context window and inference time
- Want to see and control what the AI is doing at every step
- Deal with large local datasets (big codebases, downloaded wikis, document libraries)
  where naive "load everything" approaches fail

This is not a mass-market product. It is a precision tool for people who think carefully
about how they work with AI.

---

## Competitive Landscape

Locus is local-first, workspace-native, non-editor-locked, and designed around
transparent, token-frugal navigation across code and non-code corpora.

### Aider
**What it is**: CLI coding agent, git-aware, widely used, genuinely excellent for code edits.

**Where Locus differs**:
- Aider is code-first and git-first; Locus treats code, PDFs, notes, docs, and offline knowledge bases as the same workspace problem.
- Aider's strongest workflow is direct file editing inside a repo; Locus invests more in persistent workspace indexing and query tools before reads.
- Aider's undo story leans on git; Locus also has app-level checkpoints for non-git or mixed workspaces.

### Cline / Claude Code / OpenCode
**What they are**: capable agentic coding tools with file access, terminal access, permissions, planning modes, MCP/extensibility, and rapidly growing ecosystems.

**Where Locus differs**:
- They generally optimize for high-capability cloud models first, with local models as an option. Locus optimizes for local models as the default constraint.
- They are primarily coding products; Locus's product boundary is the local workspace, including document libraries and offline knowledge bases.
- Locus's tool manifest, capability toggles, paginated reads, retrieval strategy, and compaction work are explicitly tuned for small and mid-sized local context windows.
- Locus is designed as a standalone local core with multiple possible frontends, not as an editor extension first.

### Continue.dev / Cursor / other AI editors
**What they are**: editor-integrated assistants with codebase awareness, chat, edits, and model/provider integrations. Some include indexing and retrieval.

**Where Locus differs**:
- Locus is not locked to VS Code, JetBrains, or a proprietary editor surface.
- Locus does not require source text to leave the machine for its default workflow.
- Locus exposes the workspace index, tool calls, approvals, and context budget as inspectable product surfaces rather than hiding them behind editor magic.
- Locus is built for non-code workspaces as first-class test targets, not as a side effect of file search.

### LM Studio / Jan / local chat frontends
**What they are**: excellent local model runners and chat surfaces.

**Where Locus differs**:
- They run the model; Locus operates a workspace.
- Locus gives the model indexed file/search/tool access, checkpointing, approvals, sessions, and per-workspace state.
- Locus can use LM Studio as the backend, but the product value is the agent and workspace layer above it.

### Obsidian + AI plugins
**What it is**: Markdown notebook with a large plugin ecosystem and some AI integrations.

**Where Locus differs**:
- Obsidian is strongest inside a Markdown vault; Locus works with arbitrary folders and file types.
- Locus includes code-agent primitives such as terminal commands, diffs, checkpoints, AST/symbol search, and tool approval.
- Locus is not asking the user to adopt a note-taking convention before their data becomes useful.

---

## Locus's Core Differentiators

### 1. Local-First Workspace Operation

Locus is built around the assumption that private data should stay on the user's machine
unless the user explicitly points it at a remote LLM endpoint. The default path is local
LLM plus local index plus local tools.

### 2. Structured Indexing Across Code And Documents

Locus builds and maintains a live, structured index of the workspace: file tree,
full-text search, code symbols, document headings, and optional semantic vectors.
The agent queries that index surgically instead of loading whole folders into context.

The differentiator is not "nobody else indexes." The differentiator is that indexing is
the center of the product, spans code and non-code corpora, and is paired with explicit
tool use instead of invisible context stuffing.

### 3. Any Data, Not Just Code

Locus is for:
- Code projects
- Locally downloaded reference material
- Private document folders
- Personal notes and project notebooks
- Offline knowledge bases

The same interaction model should serve a C++ repo, a PDF folder, and a local wiki.
This claim needs constant proof through test workspaces and demos, not just philosophy.

### 4. Understanding And Control As A Feature

Most tools hide the AI's process and ask you to trust the result. Locus optimizes for
"the user understands what's happening and shapes how it works." The aim is comprehension
and tunability.

- Everything the agent does is observable: tool calls, the system prompt and context
  breakdown, diffs, activity, and metrics are surfaced rather than hidden.
- You tune behavior in settings: permission presets (auto-run-everything through
  ask-before-edits), capability toggles, prompt-cost and compaction profiles, and
  per-model presets.
- Context compaction is visible and directable.
- LOCUS.md lets the user permanently shape behavior for a workspace.

This is not a limitation. It is the design goal: an agent you can reason about and steer,
at whatever level of oversight you choose.

### 5. Token Efficiency As Engineering Discipline

On local hardware, every token has a real latency cost. Locus treats token usage as
an engineering constraint:
- Tools and targeted retrieval over context pre-loading
- Paginated reads instead of full-file dumps
- Capability toggles to keep unused tools out of the manifest
- Persistent index so workspace knowledge survives compaction
- Context window and prompt/generation split visible to the user

### 6. Local Model UX Is Product-Critical

The local model setup is not incidental plumbing. Model selection, tool-call format,
server compatibility, timeout behavior, sampler defaults, and context-budget feedback
are part of the user experience. If a user cannot get a reliable local model loop running,
the privacy and offline story never gets a chance to matter.

### 7. Your Workstation, Accessible From Anywhere

The planned Connected milestone keeps the same privacy thesis: the user's workstation
does the compute and serves thin clients over the local network. The goal is remote
operation without routing private workspace data through a cloud service.

---

## What Locus Is Not Trying To Be

- It is not a "set it and forget it" automation system.
- It is not optimized for users who do not want to understand what the AI is doing.
- It is not a cloud product with a local mode bolted on.
- It is not a feature-complete replacement for a full IDE.
- It is not trying to win on raw capability. It is trying to win on trust, locality, and efficiency.
