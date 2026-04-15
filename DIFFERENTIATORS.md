# What Makes Locus Different

> This document exists to keep the project honest as it evolves.
> When a feature decision feels unclear, come back here and ask:
> does this serve the people we're actually building for?

---

## Who This Is For

People who:
- Have private data (code, documents, notes, archives) they won't or can't send to the cloud
- Want AI assistance that works when the internet is down
- Are frustrated that existing tools waste their local LLM's limited context window
- Want to understand and control what the AI is doing at every step — not watch it run free
- Deal with large local datasets (big codebases, downloaded wikis, document libraries)
  where naive "load everything" approaches simply don't work

This is not a mass-market product. It is a precision tool for people who think carefully
about how they work with AI.

---

## The Competitive Landscape

### Aider
**What it is**: CLI coding agent, git-aware, widely used, genuinely excellent for coding.

**What it doesn't do**:
- No workspace index — it feeds whole files into context, which breaks on large codebases
- Coding only — no use for a Wikipedia dump, a folder of PDFs, a private notebook
- Automation-first — the design philosophy is "let it run", not "user decides each step"
- No way to efficiently navigate a codebase you don't already know

**Locus's answer**: Smart index-first navigation. Works on any data type, not just code.
User is in control of every tool call.

---

### Continue.dev
**What it is**: VS Code extension, supports local LLMs, open source, well-maintained.

**What it doesn't do**:
- No workspace index — context stuffing is the primary mechanism
- VS Code-locked — useless outside VS Code, useless for non-code workspaces
- No large-dataset use cases
- Limited user control over what gets fed into context

**Locus's answer**: Standalone app that works for any workspace type.
Index-driven retrieval instead of context stuffing.

---

### Cursor / GitHub Copilot / Codeium
**What they are**: Cloud-based AI editors. Powerful. Polished.

**What they don't do**:
- Your data leaves your machine — always
- Useless without internet
- No support for non-code private data
- You cannot see or control what they feed to the model

**Locus's answer**: 100% local by default. Everything stays on your machine.
Works offline. Full transparency into every LLM call.

---

### Jan.ai / LM Studio (chat mode)
**What they are**: Local LLM frontends — chat interface for running models locally.

**What they don't do**:
- No workspace concept — no awareness of any folder or file system
- No agent capabilities — no tools, no file access, no search
- No index — you have to manually paste whatever context you want
- Every session starts from zero

**Locus's answer**: Persistent, indexed workspace awareness. The LLM knows where it is
and can navigate there efficiently without the user doing manual copy-paste.

---

### Obsidian + AI plugins
**What it is**: Markdown notebook with a plugin ecosystem, some AI integrations.

**What it doesn't do**:
- Locked to Obsidian's vault format and file conventions
- AI plugins are mostly chat with a single note, not workspace-aware agents
- No code-specific tools (terminal, build, syntax-aware search)
- Markdown-only — no use for code projects or arbitrary file types

**Locus's answer**: Works with any folder, any file type, no convention imposed on the user.
The same tool serves a code project and a personal notebook.

---

## Locus's Core Differentiators

### 1. The Workspace Index
Every other tool either blindly dumps files into context or has no workspace awareness at all.
Locus builds and maintains a live, structured index of the workspace — file tree, code symbols,
full-text search, headings — and the agent queries it surgically instead of loading whole files.

This is what makes Locus usable on large datasets (100k-file codebases, Wikipedia dumps)
where every other tool grinds to a halt or produces garbage.

### 2. Any Data, Not Just Code
Aider is for code. Continue.dev is for code. Locus is for:
- Code projects
- Locally downloaded reference material (Wikipedia, docs, books)
- Private document folders (PDFs, reports, notes)
- Personal idea and project notebooks

The index, the tools, and the agent design are all data-type-agnostic.

### 3. User Control as a First-Class Feature
Most tools optimize for "let the AI do it." Locus optimizes for "user decides, AI executes."
- Every tool call is visible before execution
- User approves, modifies, or rejects each action
- Context compaction is presented as a choice, not a silent background event
- LOCUS.md lets the user permanently shape how the AI behaves in this workspace

This is not a limitation — it is the design goal. The user is the architect; the AI is the
heavy-lifter that does exactly what the user directs.

### 4. Token Efficiency as Engineering Discipline
On local hardware, every token has a real cost in inference time.
Locus treats token usage as a first-class engineering concern:
- Tools and targeted retrieval over context pre-loading
- Paginated reads — never full-file dumps
- Tool results summarized before injection
- Context window always visible to the user
- Compaction strategies that the user controls

### 5. Your Workstation, Accessible From Anywhere
Locus Core runs as a daemon on your PC. Connect to it from your phone, tablet, or a
browser on another machine — over your local network, without routing through any cloud.
You get the full agent experience remotely: browse code, ask questions, approve tool calls,
kick off builds. Your PC does all the compute; the remote device is just a thin client.

Most AI coding tools are either bolted into a specific editor or live entirely in the cloud.
Locus is neither — it's a headless process on your machine that any frontend can attach to.
This is built for engineers who want to operate their workspace, not just chat with it.
You see every tool call, every argument, every result — whether you're at the desk or on
the couch with your phone.

### 6. No Internet Required
Core functionality works with zero network access.
This is both a privacy guarantee and a reliability guarantee.

---

## What Locus Is Not Trying to Be

- It is not a "set it and forget it" automation system
- It is not optimized for users who don't want to understand what the AI is doing
- It is not a cloud product with a local mode bolted on
- It is not a feature-complete replacement for a full IDE
- It is not trying to win on raw capability — it is trying to win on trust and efficiency
