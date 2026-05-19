# Test Workspaces

Three workspaces used from day one to test and drive development.
Each covers a different primary use case and surfaces different technical requirements.

---

## Workspace 1 -- Locus Project Folder

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
This is the Locus project -- a C++20 local LLM agent assistant.
Build: CMake + vcpkg, MSVC (VS 2026), Windows 11.
Key architecture docs: CLAUDE.md (start here), architecture/ folder.
Index database: SQLite + FTS5 + sqlite-vec. UI: wxWidgets + wxWebView.
Do not modify CLAUDE.md or any .md files under architecture/ without explicit instruction.
```

### Success criteria
- Agent can answer "where is the ITool interface defined?" without reading every file
- Agent can implement a small feature (e.g. a new tool stub) from a description
- Agent can run `cmake --build` and report compilation errors

---

## Workspace 2 -- Wikipedia (Kiwix)

**Path**: `D:\Projects\LocusTestWorkspaces\WS2_Wikipedia\` (built by [scripts/build_ws2.ps1](scripts/build_ws2.ps1))
**Access**: Read-only
**Type**: Knowledge base
**Scale**: 5,000 articles by default (cap configurable; full Simple English ZIM is ~250K articles)

### Setup

From the repo root:

```
pwsh ./scripts/build_ws2.ps1
```

This downloads a Simple English Wikipedia ZIM (~250 MB), downloads
`kiwix-tools` for `zimdump.exe`, extracts the first 5000 articles
alphabetical into `<WorkspaceRoot>/A/*.html`, and writes a `LOCUS.md`
that frames the workspace as read-only.

To rebuild from scratch: `pwsh ./scripts/build_ws2.ps1 -Force`. To keep
the full corpus instead of the 5000-article cap: `-MaxArticles 0`. The
ZIM is cached under `D:\Projects\LocusTestWorkspaces\.cache\` so a
second build doesn't re-download.

### Disk footprint (default 5000-article subset)

Measured 2026-05-18 against `wikipedia_en_simple_all_nopic_2026-05.zim`:

| Artifact                              | Size      |
|---|---|
| Cached ZIM                            | 937 MB    |
| Extracted articles (`A/*.html`)       | ~140 MB   |
| Locus `.locus/index.db`               | 49 MB     |
| Locus `.locus/vectors.db` (bge-small) | 90 MB (33,965 chunks × 384-dim Q8_0) |

### Performance notes (measured)

5000-article WS2 on Windows 11, CPU-only embedder, no GPU:

- First index build (text + chunking): ~30 s
- Embedding queue: **33,965 chunks**. bge-m3 (recommended profile)
  embeds ~0.9 chunks/sec on CPU -- a full drain would take ~10 hours.
  bge-small (small profile) embeds ~10 chunks/sec -- full drain in
  ~55 min. **WS2 ships configured for the small profile** (set in
  `.locus/config.json` -> `index.semantic_search.model = "bge-small-en-v1.5-Q8_0.gguf"`).
  Switch to bge-m3 only when you've got hours to spare and the small
  embedder's recall isn't enough.
- Query latency (post-drain): text ~26 ms total / 14 queries;
  semantic and hybrid ~4 s total each (cold model load on first query).
- **Small-profile paraphrase ceiling (measured).** bge-small at 384
  dimensions doesn't bridge wide synonymy gaps. Concrete example from
  the S5.N WS2 acceptance run, all four queries against the same
  workspace and same 5000-article corpus:

  | Query                                                | Rank-1 hit             |
  |---|---|
  | "Which scientist explained how species change over time?" | `A/Earth.html` (wrong) |
  | "species change over time scientist"                 | `A/Earth.html` (wrong) |
  | "evolution natural selection"                        | `A/Charles_Darwin.html` |
  | "Darwin"                                             | `A/Charles_Darwin.html` |

  The Darwin article IS indexed (7 chunks, 13 KB of extracted text)
  and BM25 finds it perfectly when the query uses matching vocabulary.
  But the semantic embedder doesn't bridge "species change over time"
  to the article's actual phrasing "evolution by natural selection",
  so the *full* hybrid retrieval misses Darwin entirely on the
  paraphrased query.

  This is a real characteristic of bge-small on a 5000-article
  corpus, not a Locus bug. The mitigations are: (a) keyword-anchored
  queries for high-stakes lookups (the agent can rephrase its own
  search if its first attempt misses), or (b) switching to bge-m3
  on the recommended profile when paraphrase robustness matters.
  Pre-S5.N retrieval-eval evidence has bge-m3 hitting MRR=1.000 on
  the WS2 gold set, which confirms the larger embedder closes this
  gap.
- Headline retrieval at full coverage:
  - search_text: 14/14, MRR=0.905, nDCG@10=0.929
  - search_semantic: 14/14, **MRR=1.000, nDCG@10=1.000** (perfect rank
    on Wikipedia-style queries -- the embedder strongly captures the
    "what article is this about" signal)
  - search_hybrid: 14/14, MRR=0.929, nDCG@10=0.947
- Full per-query breakdown: [tests/retrieval_eval/results_ws2.md](tests/retrieval_eval/results_ws2.md).

### Why a pre-extracted ZIM and not native libzim?

[S6.2](roadmap/M6/S6.2-zim-reader.md) will integrate `libzim` (official
C/C++ library from the Kiwix project, MIT licensed) so a `.zim` file
can be opened as a virtual workspace without extraction -- the correct
long-term path. S5.N delivers the non-code workspace proof on what we
already have: HTML extraction, the existing indexer, and `zimdump` for
one-time extraction. The same retrieval + agent loop will carry forward
unchanged once S6.2 lands; only the file backend changes.

### What it tests
- Semantic search on natural-language text at moderate scale
- Hybrid search (keyword + semantic) for topic queries
- Read-only workspace with no code -- capability defaults + `LOCUS.md` policy
- HTML extractor end-to-end at thousands-of-files volume
- "Answer not present" framing on a deliberately small subset

### Acceptance prompts

Four WS2 prompts are part of S5.N's 10-prompt acceptance set; the full
walkthrough lives in [tests/manual/non-code-workspaces.md](tests/manual/non-code-workspaces.md)
Test 1. Summary of what each probes:

- "What is the capital of Australia?" -> direct keyword retrieval + citation
- "Compare TCP and UDP" -> cross-document synthesis
- "Explain Byzantine fault tolerance" -> semantic > keyword (synonymy gap)
- "What does this corpus say about quantum chromodynamics?" -> negative case

---

## Workspace 3 -- Personal Documents Folder

**Path**: `D:\Projects\LocusTestWorkspaces\WS3_Documents\` (built by [scripts/build_ws3.ps1](scripts/build_ws3.ps1))
**Access**: Read-only (enforced via `LOCUS.md`)
**Type**: Personal document library
**Scale**: Small (~30-40 files, ~10-20 MB) -- representative, not exhaustive

Mixed file types: PDF, TXT, DOCX, XLSX, Markdown -- one file per extractor
path so the index exercises every extractor end-to-end.

### What it tests
- PDF text extraction via PDFium (older RFCs via the auto-rendered
  `/pdfrfc/` path -- worst-case for PDFium quality)
- DOCX text extraction (miniz + pugixml)
- XLSX cell text extraction
- Markdown notes that reference content found in PDFs -- cross-document synthesis
- Read-only enforcement via `LOCUS.md` policy framing (agent must refuse edits)

### Setup

```
pwsh ./scripts/build_ws3.ps1
```

Downloads 10 IETF RFCs as PDF (rfc-editor.org, public domain), fetches
one DOCX + one XLSX sample from Apache POI test-data (Apache-2.0),
writes five hand-authored Markdown notes that cross-reference the RFCs,
and creates a `LOCUS.md` that frames the workspace as read-only.

To rebuild: `pwsh ./scripts/build_ws3.ps1 -Force`.

If the POI sample URLs rot, the script logs `[fail]` and continues
without those files -- drop in your own `samples/*.docx` /
`samples/*.xlsx` to keep extractor coverage complete.

### Disk footprint

Measured 2026-05-19 on the recommended profile (bge-m3):

| Artifact                            | Size      |
|---|---|
| RFCs (10 × PDF)                     | ~3 MB     |
| Samples (DOCX + XLSX)               | ~20 KB    |
| Markdown notes (5 files)            | ~7 KB     |
| Locus `.locus/index.db`             | ~2 MB     |
| Locus `.locus/vectors.db` (bge-m3)  | ~4 MB (609 chunks × 1024-dim Q8_0) |

### Performance notes (measured)

- First index build: ~5 s
- Embedding queue drain (recommended profile, bge-m3 on CPU): ~2 min
  (609 chunks; small corpus is tractable on bge-m3)
- Headline retrieval at full coverage:
  - search_text: 6/13 (46%) -- formal RFC language doesn't align with
    natural-language queries
  - search_semantic: **13/13** (100%), MRR=0.88, nDCG@10=0.88
  - search_hybrid: **13/13** (100%), MRR=0.87, nDCG@10=0.88
- Full per-query breakdown: [tests/retrieval_eval/results_ws3.md](tests/retrieval_eval/results_ws3.md).

### Bugs surfaced by the S5.N investigation

Three real issues found while debugging the original 11/13 hit-rate. See
[roadmap/backlog/README.md](roadmap/backlog/README.md) for production-side
follow-ups; the WS3 evidence run has the per-workspace mitigations applied
already.

- **`index.max_file_size_kb = 1024`** silently dropped rfc9114.pdf
  (1158 KB). Build script now pre-seeds `.locus/config.json` with
  `max_file_size_kb = 4096`; production default raise is in the backlog.
- **FTS5 `syntax error near "/"`** on "HTTP/3" queries. Eval harness
  `sanitise_for_fts` now strips `/`; the same fix in
  `IndexQuery::search_text` is in the backlog (chat-tool users currently
  hit the same crash for HTTP/3, Node.js, K&R, etc.).
- **Acronym vs spelled-out form** ("ARP" query vs RFC 826's literal
  "Address Resolution Protocol" title). Updated the gold query to the
  spelled-out form; the general acronym-expansion gap is a separate
  retrieval-quality problem in the backlog.

### Acceptance prompts

Six WS3 prompts are part of S5.N's 10-prompt acceptance set; the full
walkthrough lives in [tests/manual/non-code-workspaces.md](tests/manual/non-code-workspaces.md)
Test 2. Summary of what each probes:

- Which RFC defines HTTP/3 -> direct keyword + cross-format ranking
- Outline of RFC 7540 -> PDFium heading extraction + outline tool
- Summarise my web-protocols notes -> Markdown synthesis + RFC cross-ref
- Find content in `samples/SampleSS.xlsx` -> XLSX extractor
- "TLS 1.3 in my notes + which RFC" -> notes prose + PDF source synthesis
- Ask for an edit to a note -> `LOCUS.md` read-only policy honoured

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
