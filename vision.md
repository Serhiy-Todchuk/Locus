# Vision

## Why This Exists

### Problem

Modern AI coding assistants (GitHub Copilot, Cursor, etc.) are:
- Cloud-dependent — useless when internet is down
- Privacy-hostile — your private code and data leave your machine
- Token-wasteful — context windows get bloated with irrelevant data
- Opaque — the user cannot see or control what the AI is doing step by step
- Generalist — not optimized for navigating large local datasets

Consumer-grade local LLMs are now capable enough for serious work (e.g. Gemma 4 26B A4B),
but no good tooling exists to wield them effectively, especially over large local data sets.

### Origin

Locus started as a knowledge navigation tool — inspired by Google's Project Nomad — focused
on searching and browsing large local datasets (Wikipedia dumps, document libraries, personal
archives) with AI assistance. The coding agent features came later, once it became clear that
navigating a codebase and navigating a document folder are fundamentally the same problem:
index the workspace, let the agent fetch what it needs, keep the user in control. There was
no reason to build two separate tools.

### Solution

An AI agent assistant that:
- Runs entirely locally — no internet required for core operation
- Is optimized for the constraints of local LLMs (limited context, slower inference)
- Uses tools and smart data fetches instead of dumping raw data into context
- Keeps the user in full control of every step — transparency over automation
- Builds and maintains a live index of the workspace so the AI can navigate it cheaply

---

## Goals

### Primary Goals
- **Privacy**: Your data never leaves your machine (unless you explicitly choose it)
- **Offline capability**: Emergency AI assistant when internet is down
- **Token efficiency**: Smart retrieval over brute-force context stuffing
- **User control**: User decides what happens at each step — AI proposes, user approves
- **Large-data navigation**: Navigate codebases, document libraries, wiki dumps without
  spending a full context window just to find relevant files

### Secondary Goals
- **Extensibility**: Start as standalone app, grow into VS Code plugin and beyond
- **Multi-purpose**: Coding assistant, knowledge search, document organizer, idea capture
- **Online model support**: Add Claude/OpenAI as optional backends later

---

## Philosophy

### User as Architect, AI as Heavy-Lifter

The user is the high-level think-tank. The AI handles the low-level implementation work.
This is especially true for software development:
- Planning/architecture = a separate, dedicated task/prompt
- Coding = a separate, dedicated task/prompt
- Review/refactor = another dedicated task

Full autonomy is NOT the goal. Transparent collaboration is.

### Conservative Token Usage

Every token costs inference time on local hardware. The system must:
- Use tools (file reads, targeted searches) instead of pre-loading data into context
- Retrieve only what is needed, when it is needed
- Summarize and cache intermediate results where appropriate
- Never repeat information already established in the conversation

### Everything Is Observable

The user can always see:
- What the AI is about to do
- What tools it is calling and with what arguments
- What data it retrieved
- What it is planning next

Nothing happens in a black box.

---

## Target User (v1)

**Experienced software developer / programmer** who:
- Understands LLM capabilities and limitations
- Wants fine-grained control over AI-assisted workflows
- Has private codebases, documents, or data they cannot or will not send to the cloud
- Values efficiency and transparency over push-button magic
- May work in environments without reliable internet access

Future: may expand to less technical users as the tooling matures.

---

## Use Cases

### 1. Coding Agent
Work on a local codebase. The AI can read, write, create, and search files.
It can run terminal commands (build, test, execute). User controls each step.

### 2. Knowledge Search Engine
A locally downloaded copy of Wikipedia, documentation sets, or research papers.
Ask natural-language questions; the AI searches the index — using both keyword and
semantic (meaning-based) search — and retrieves relevant passages without loading
the entire dataset into context.
Supports Kiwix `.zim` archives natively: Wikipedia stays as a single compressed file,
no extraction needed.

### 3. Document Organizer
A folder of personal documents (PDFs, notes, reports). Find, summarize, cross-reference,
or reorganize them with AI assistance.

### 4. Idea & Project Notebook
A private folder of markdown notes, project sketches, and ideas. The AI helps you find
connections, expand thoughts, and organize material.

---

### 5. Your Workstation in Your Pocket
Locus Core runs as a background process on your PC. Any frontend — desktop, browser,
or mobile — can connect to it over the local network. This means you can open your
phone on the couch, on a train, or in another room, and work on the same project
that's sitting on your desktop: read code, ask the agent questions, approve tool calls,
run builds — everything short of physically being at the keyboard.

Your PC does the heavy lifting (LLM inference, file I/O, indexing). The phone is just
a thin window into the workspace. Nothing leaves your network, and the full tool
approval flow works exactly the same as on desktop — you see every action, approve
or reject it, and stay in control.

This is built for engineers who actually care about what's happening under the hood.
You're not typing prompts into a black box and hoping for the best — you're operating
your workspace remotely with full visibility into every step the agent takes.

---

## Non-Goals (v1)

- Full automated "build me an app" pipeline (future)
- Multi-agent orchestration (future)
- Cloud sync or data leaving the machine
- Voice interface
