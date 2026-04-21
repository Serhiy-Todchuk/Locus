# Workspace Index Subsystem

> The workspace index is one of the most critical components.
> It is what allows the AI to navigate large workspaces without burning tokens.

---

## Purpose

The index answers questions like:
- "Which files in this codebase relate to authentication?"
- "Where is the `UserService` class defined?"
- "Show me all markdown files modified in the last week"
- "What Python files import `asyncio`?"
- "Find the section about 'Byzantine fault tolerance' in this Wikipedia dump"
- "What code in this repo handles memory allocation?" ← requires semantic search

Without an index, the agent would have to read files blindly — catastrophic for large workspaces.
With an index, the agent issues a cheap query and gets back ranked, targeted results.

**The index is NEVER updated by the LLM.** Only by deterministic, algorithmic processes.
This keeps it fast, accurate, and free from hallucination contamination.

---

## What Gets Indexed

### All Files
- Absolute path, relative path from workspace root
- File size, last modified timestamp, extension, MIME type, binary/text flag

### Text Files (additional)
- Full text stored for FTS indexing
- Line count, detected natural language
- Headings (Markdown `#`, HTML `<h1>`, etc.) with line numbers
- First N characters as a preview snippet

### Code Files (additional, language-aware via Tree-sitter)
- Detected programming language
- Top-level declarations: classes, functions, methods, interfaces, enums
  - Name, line range, signature, parent name (for methods)
- Import/require/include statements

### Documents (additional)
- PDF: extracted text, title, author
- DOCX: extracted text, headings
- (Others added as needed)

---

## Index Database

**SQLite** with two search layers:
1. **FTS5** — keyword / BM25 ranked full-text search
2. **sqlite-vec** — vector similarity search (semantic)

Storage is **split across two files** in `.locus/`:

- `index.db` — always present. Holds the deterministic skeleton: files, FTS5,
  symbols, headings. Opened by the main `Database` connection.
- `vectors.db` — present only when semantic search is enabled. Holds `chunks`
  and the `vec0` virtual table `chunk_vectors`. Opened by a separate `Database`
  connection; loads the sqlite-vec extension only here.

The split gives three things:

1. **Concurrent writes** — each file has its own SQLite WAL. The indexer's FTS /
   symbols writes no longer serialise behind the embedding worker's vector
   inserts. Background embedding can't stall foreground re-indexing.
2. **A detachable cache** — vectors are derived from chunk text + embedding
   model. Changing models or reclaiming disk is one `rm vectors.db`; the main
   index is untouched.
3. **Zero cost when semantic is off** — if `semantic_search.enabled = false`
   (or the model file is missing), `vectors.db` is never created and the
   sqlite-vec extension is never initialised.

### Schema — `index.db`

```sql
-- Core file registry
CREATE TABLE files (
    id          INTEGER PRIMARY KEY,
    path        TEXT UNIQUE NOT NULL,   -- relative path from workspace root
    abs_path    TEXT NOT NULL,
    size_bytes  INTEGER,
    modified_at INTEGER,               -- unix timestamp
    ext         TEXT,
    is_binary   INTEGER,               -- 0/1
    language    TEXT,                  -- 'cpp', 'markdown', 'python', etc.
    indexed_at  INTEGER
);

-- Full-text search over file contents (FTS5 + BM25)
CREATE VIRTUAL TABLE files_fts USING fts5(
    path UNINDEXED,
    content,
    tokenize='porter unicode61'
);

-- Code symbols (Tree-sitter extracted)
CREATE TABLE symbols (
    id          INTEGER PRIMARY KEY,
    file_id     INTEGER REFERENCES files(id),
    kind        TEXT,                  -- 'function', 'class', 'method', 'struct', etc.
    name        TEXT NOT NULL,
    line_start  INTEGER,
    line_end    INTEGER,
    signature   TEXT,
    parent_name TEXT                   -- containing class/namespace for methods
);
CREATE INDEX symbols_name ON symbols(name);
CREATE INDEX symbols_file ON symbols(file_id);

-- Document headings / outline
CREATE TABLE headings (
    id          INTEGER PRIMARY KEY,
    file_id     INTEGER REFERENCES files(id),
    level       INTEGER,               -- 1–6
    text        TEXT,
    line_number INTEGER
);
```

### Schema — `vectors.db` (optional)

```sql
-- Semantic chunks. file_id references files(id) in index.db — this is a
-- cross-file logical reference, not a FK (orphaned chunks are acceptable
-- since they'll be cleaned up on the next re-index of that file).
CREATE TABLE chunks (
    id          INTEGER PRIMARY KEY,
    file_id     INTEGER NOT NULL,
    chunk_index INTEGER,
    start_line  INTEGER,
    end_line    INTEGER,
    content     TEXT NOT NULL
);
CREATE INDEX chunks_file ON chunks(file_id);

-- Vector table (sqlite-vec extension — loaded only on this DB).
CREATE VIRTUAL TABLE chunk_vectors USING vec0(
    chunk_id    INTEGER PRIMARY KEY,
    embedding   FLOAT[384]
);
```

Semantic search becomes a two-step read: query `vectors.db` for the top-K
nearest `chunk_id` → `file_id`, then resolve `file_id` → `path` from
`index.db`. Both lookups are cheap (tiny K, single-row PK fetches).

---

## Chunking Strategy

Chunking splits files into meaningful units for embedding. The goal is that each chunk is
semantically coherent — a complete thought, function, or section — not an arbitrary slice.

### Code files (Tree-sitter)
- Each **function or method body** = one chunk (most natural semantic unit)
- Each **class definition** = one chunk (its header/doc comment + member list, not bodies)
- Functions longer than `chunk_size_lines` (configurable, default 80) are split at a
  sub-block boundary (inner `if`/`for` blocks), with `chunk_overlap_lines` (default 10)
  lines repeated at boundaries to preserve context

### Document files (Markdown, plain text)
- Split at **heading boundaries** (H2 or H3 level, configurable)
- Sections longer than `chunk_size_lines` use a **sliding window** with overlap
- Short sections (< 3 lines) are merged with the next section

### Fallback (unrecognised file types)
- Fixed sliding window: 256 lines per chunk, 32 lines overlap

---

## Embedding Pipeline

### Embedding model

Embedding runs **in-process** via the llama.cpp C API.
No dependency on LM Studio being available. No VRAM used.

**Default model: `all-MiniLM-L6-v2` (Q8_0 GGUF)**
- Size: ~25MB GGUF file stored in `models/` (bundled next to the executable)
- Output dimension: 384
- Speed: ~5,000 chunks/minute on CPU (typical)
- Quality: good for code and English text; sufficient for most use cases
- Tokenisation: WordPiece vocab embedded in the GGUF — no auxiliary `vocab.txt` needed

**Upgrade option: `nomic-embed-text-v1.5` (GGUF)**
- Size: ~280MB
- Output dimension: 768
- Significantly better quality for long documents and cross-lingual content
- Slower (~800 chunks/minute on CPU)

Swapped from ONNX Runtime in April 2026: vcpkg's static-CRT onnxruntime dropped ONNX schema
registration constructors at link time, causing "invalid model" failures. llama.cpp is a
single-file C library with no linker-sensitive static registrations and ships a correct
tokenizer. See [tech-stack.md § 5](tech-stack.md) for details.

Model is configured per workspace. Changing the model triggers a full re-embedding pass.

### Embedding build order
1. FTS indexing runs first (faster, provides value immediately)
2. Embedding runs in a background thread at lower priority
3. Agent can already use keyword search while embedding completes
4. Progress shown separately in UI: "FTS: done | Vectors: 12,400 / 45,000 chunks"

### Incremental updates
- When a file changes: delete all chunks + vectors for that file, re-chunk, re-embed
- Only the changed file is processed — no full rebuild
- Very large files (> `max_file_size_kb`) are not embedded (only FTS indexed)

---

## Search: Hybrid BM25 + Semantic

Neither keyword nor semantic search alone is best. They complement each other:
- **BM25 (FTS5)**: great for exact names, identifiers, error codes, technical terms
- **Semantic**: great for concepts, intent, descriptions ("code that manages memory")

**Hybrid search uses Reciprocal Rank Fusion (RRF)** to merge the two ranked lists:

```
rrf_score(doc) = Σ  1 / (60 + rank_i(doc))
```

Each result list contributes a term. Documents appearing in both lists score higher.
k=60 is the standard constant — robust without tuning.

```
Query: "memory allocation management"
             │
    ┌────────┴────────┐
    │                 │
  FTS5 (BM25)    Semantic (cosine)
  rank list       rank list
    │                 │
    └────────┬────────┘
             │ RRF merge
             ▼
    Final ranked results
    { path, line_start, label, snippet, score }
```

### `search_hybrid(query, options?)` — default search tool
Runs both and merges. Returns ranked snippets, not full files.
Options: filter by path prefix, extension, language, symbol kind.

### `search_text(query, options?)` — keyword only
FTS5 BM25. Used when semantic is disabled or for exact-match queries.

### `search_symbols(name, kind?, language?)` — code symbol lookup
Exact + prefix match on the `symbols` table. Fast, no embedding needed.

### `search_semantic(query, options?)` — vector only
Pure cosine similarity. Useful when exact terms are unknown.

---

## Query API (Agent Tools)

The agent never touches SQLite directly. High-level tools only:

| Tool | Search type | Token cost | When to use |
|---|---|---|---|
| `search_hybrid` | BM25 + semantic | Low | Default — most queries |
| `search_text` | BM25 keyword | Very low | Exact names, IDs, error strings |
| `search_symbols` | Symbol table | Minimal | "Where is class X defined?" |
| `get_file_outline` | Index metadata | Minimal | Understand a file before reading it |
| `list_directory` | File registry | Minimal | Explore folder structure |

All tools return **ranked snippets with locations** — never full file contents.
Full content is fetched only via `read_file` when the agent decides it's necessary.

---

## Workspace File Ownership

| Path | Owner | Rule |
|---|---|---|
| `LOCUS.md` (workspace root) | **User** | User creates and edits. Locus only reads. LLM cannot write it. |
| `.locus/` (entire folder) | **Locus app** | Created and managed by Locus. User should not edit contents manually. |

---

## Index Config (per workspace, stored in `.locus/config.json`)

```json
{
  "index": {
    "exclude_patterns": ["node_modules/**", ".git/**", "*.lock", "dist/**"],
    "max_file_size_kb": 1024,
    "code_parsing_enabled": true,
    "semantic_search": {
      "enabled": false,
      "model": "all-MiniLM-L6-v2",
      "model_path": "models/all-MiniLM-L6-v2.Q8_0.gguf",
      "dimensions": 384,
      "chunk_size_lines": 80,
      "chunk_overlap_lines": 10
    }
  }
}
```

Semantic search is **opt-in per workspace** (disabled by default) because:
- Initial embedding of a large workspace takes time and CPU
- Small workspaces benefit little from it — BM25 is already excellent there
- Some workspaces (binary assets, build output) have no useful text to embed

---

## Open Questions

- How to handle symlinks and network mounts? (skip by default, configurable)
- Language detection for natural language text: `cld3` (Google, C++) or heuristic by extension
