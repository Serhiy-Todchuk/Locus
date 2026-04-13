# Test Workspaces

Three workspaces used from day one to test and drive development.
Each covers a different primary use case and surfaces different technical requirements.

---

## Workspace 1 — Locus Project Folder

**Path**: `d:\Projects\AICodeAss\`
**Access**: Read-write
**Type**: Code project
**Scale**: Small (grows as development proceeds)

The most useful test workspace: Locus developing itself.
Every feature implemented can be immediately exercised on its own codebase.

### What it tests
- Code symbol search (C++ classes, functions across files)
- File read/write/create by agent
- Terminal tool (CMake build, running tests)
- LOCUS.md injecting project-specific conventions into every request
- Diff view for AI-proposed code changes

### Expected LOCUS.md
```markdown
This is the Locus project — a C++20 local LLM agent assistant.
Build: CMake + vcpkg, MSVC (VS 2026), Windows 11.
Key architecture docs: CLAUDE.md (start here), architecture/ folder.
Index database: SQLite + FTS5 + sqlite-vec. UI: TBD (ImGui likely).
Do not modify CLAUDE.md or any .md files under architecture/ without explicit instruction.
```

### Success criteria
- Agent can answer "where is the ITool interface defined?" without reading every file
- Agent can implement a small feature (e.g. a new tool stub) from a description
- Agent can run `cmake --build` and report compilation errors

---

## Workspace 2 — Wikipedia (Kiwix)

**Path**: TBD (location of extracted content or ZIM file)
**Access**: Read-only
**Type**: Knowledge base
**Scale**: Very large — full English Wikipedia is ~22 million chunks when embedded

### The ZIM format problem

Kiwix distributes Wikipedia as `.zim` files — a single compressed archive containing
all articles. A full English Wikipedia ZIM is ~90GB compressed (~21M articles).

**Options for Locus:**

#### Option A — Native ZIM support (recommended long-term)
Integrate `libzim` (official C/C++ library from the Kiwix project, MIT licensed).
- Articles are read directly from the ZIM archive without extraction
- No disk space wasted on decompression
- Random access by article title or URL path
- Locus treats the ZIM file as a virtual workspace — the "files" are articles

Technical requirements:
- libzim as a vcpkg dependency (or bundled)
- A ZIM-aware indexer that iterates articles via libzim API
- Article text extracted as plain text / stripped HTML
- FTS5 + embedding indexed over article content
- `list_directory` maps to ZIM namespace/category structure

#### Option B — Kiwix HTTP server (easier short-term)
Run `kiwix-serve` locally, Locus fetches articles via HTTP.
- No libzim dependency
- Locus treats it like a web search tool pointed at localhost
- Loses the "fully offline single binary" advantage
- Good for prototype testing before libzim integration

#### Option C — Pre-extracted HTML files
Extract the ZIM to individual HTML files (kiwix-tools can do this).
- Locus works with a folder of HTML files — no special support needed
- Cons: requires ~3x the disk space, millions of small files, slow filesystem
- Only viable for a small subset (e.g. a topic-specific ZIM like "Wikipedia Medicine")

**Plan**: Use Option B (kiwix-serve) for early testing. Implement Option A (libzim)
as a planned feature — it is the correct long-term solution and a meaningful differentiator.

### What it tests
- Semantic search on natural language text at massive scale
- Hybrid search (keyword + semantic) for topic queries
- Graceful handling of a read-only workspace with no code
- Very large file/article counts — index build time and query performance
- "Find me everything about Byzantine fault tolerance" type queries

### Expected LOCUS.md
```markdown
This is an offline copy of English Wikipedia sourced from Kiwix.
All content is read-only — do not attempt to write or modify anything.
Articles cover all topics in the English Wikipedia as of the ZIM snapshot date.
Prefer search_hybrid for topic questions. For specific article titles, use search_text.
This workspace has no code — do not suggest code-related tools.
```

### Success criteria
- "What is the capital of Mongolia?" → finds and returns the correct article section
- "Explain Byzantine fault tolerance" → finds the relevant article and summarizes
- "What articles relate to Vulkan graphics API?" → returns ranked relevant articles
- Queries return in < 2 seconds after index is built

---

## Workspace 3 — Personal Documents Folder

**Path**: `C:\Users\serhi\Documents\` (or a curated subfolder)
**Access**: Read-only (or very cautious read-write)
**Type**: Personal document library
**Scale**: Medium — depends on contents, likely hundreds to low thousands of files

Mixed file types: PDF, DOCX, XLSX, images, text files, etc.

### What it tests
- PDF text extraction (requires a PDF library — `pdfium` or `poppler`)
- DOCX text extraction (requires XML parsing of `.docx` ZIP structure)
- XLSX: at minimum, extract cell text (no formula evaluation needed)
- Handling files with no extractable text (images, encrypted PDFs)
- Read-only enforcement — agent must never propose writes to personal documents
- Mixed language content (if documents are in multiple languages)

### File format support needed
| Format | Extraction method | Priority |
|---|---|---|
| `.md`, `.txt`, `.rst` | Direct read | Already handled |
| `.pdf` | pdfium or poppler (C API) | High |
| `.docx` | Unzip + parse `word/document.xml` | High |
| `.xlsx` | Unzip + parse `xl/sharedStrings.xml` + sheets | Medium |
| `.html`, `.htm` | Strip tags, extract text | Medium |
| `.csv` | Direct read as text | Low |
| Images, audio, video | Filename + metadata only | Low |
| Encrypted / DRM PDF | Skip, log as unindexable | Required |

### Expected LOCUS.md
```markdown
This is a personal documents folder. All files are read-only.
Never propose creating, editing, or deleting any file in this workspace.
Documents are in English. Mixed types: PDF, DOCX, text files.
Focus on finding and summarizing relevant content from existing documents.
```

### Success criteria
- "Find my notes about [topic]" → returns relevant document sections
- "What PDFs do I have related to tax?" → finds relevant files by content
- "Summarize the key points of [document name]" → reads and summarizes correctly
- Encrypted or unreadable files are skipped gracefully with a logged warning

---

## Cross-Workspace Requirements These Expose

| Requirement | Driven by |
|---|---|
| ZIM file support via libzim | Workspace 2 |
| PDF text extraction | Workspace 3 |
| DOCX text extraction | Workspace 3 |
| Read-only mode hard enforcement | Workspace 2, 3 |
| Graceful skip of unreadable files | Workspace 3 |
| Very large index (millions of chunks) performance | Workspace 2 |
| Natural language (non-code) semantic search | Workspace 2, 3 |
| Code + terminal tools | Workspace 1 |
| LOCUS.md per-workspace behavior tuning | All three |
