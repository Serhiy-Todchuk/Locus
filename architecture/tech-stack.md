# Technology Stack

> **Decided: C++20 core.** Windows 11 first, cross-platform in mind for the future.
> This document lists decided choices with rationale. Future frontend options are kept
> for reference but are not the current build target.

---

## 1. Core Language — C++20

**Decided.** C++ is the right choice for Locus core:
- User's native language — fastest iteration, deepest control
- All key dependencies (SQLite, Tree-sitter, libzim, pdfium) are C/C++ native
- Zero runtime overhead, instant startup, minimal memory footprint
- Direct control over threading, memory, and performance-critical paths
- vcpkg handles dependencies cleanly on Windows

**Build system**: CMake + vcpkg.

---

## 2. Frontend — v1: wxWidgets + wxWebView

**Decided for v1** — deferred until after CLI prototype.

- Native widgets via wxWidgets — proper text selection/copy, system tray, file dialogs, all built-in
- Chat display: `wxWebView` (Edge/WebView2, OS-provided — not bundled Chromium, not Electron)
- Markdown rendering: `md4c` (C library) converts markdown → HTML, injected into WebView
- Code syntax highlighting: Prism.js (~20KB) embedded in WebView's HTML template
- Streaming LLM output: `wxTimer` flushes token buffer via `wxWebView::RunScript()` (DOM append)
- Dockable layout: `wxAuiManager` for sidebar + chat + right panel
- Tool args display: `wxStyledTextCtrl` (Scintilla) with JSON lexer
- Result: native-looking app, zero external runtime, full text editing capabilities

wxWidgets connects to Core via the **C++ direct interface** (`IFrontend` / `ILocusCore`
virtual calls, same process, zero overhead). Agent thread events are marshalled to the
UI thread via `wxQueueEvent()` + custom `wxThreadEvent` types. See [overview.md](overview.md).

---

## 3. Frontend Architecture

Three C++ frontends, all implementing `IFrontend` and linking `locus_core`:

| Frontend | Location | Purpose |
|---|---|---|
| **CLI** | `src/frontends/cli_frontend.*` | Terminal REPL, y/n tool approval, M0 prototype |
| **wxWidgets** | `src/frontends/wx/*` (M1) | Desktop GUI with system tray, chat UI, tool panels |
| **CrowServer** | `src/frontends/crow/*` (M3) | HTTP/WebSocket server for external clients |

External clients connect to CrowServer — they are not C++ frontends themselves:

| Client | Technology | Connection |
|---|---|---|
| **Browser** | HTML/CSS/JS, served by Crow at `GET /` | WebSocket + HTTP, local or LAN |
| **VS Code extension** | TypeScript, ~200 lines | WebSocket to CrowServer |
| **Mobile app** | Future | WebSocket over LAN |

Multiple frontends and clients can be active simultaneously on the same session.

---

## 4. Index Database

**SQLite + FTS5 + sqlite-vec** — all in one `.locus/index.db` file.

- SQLite C API — native, zero overhead from C++
- **FTS5**: BM25-ranked full-text search, porter stemming, unicode tokenizer
- **sqlite-vec**: vector virtual table extension for semantic search
  - Cosine similarity, exact + approximate KNN
  - Loaded as a runtime extension alongside FTS5 in the same database

No server, no config, single file, zero external process.

---

## 5. Semantic Embeddings

**llama.cpp C API** — in-process CPU inference, no dependency on LM Studio being up.

- llama.cpp, MIT, vcpkg: `llama-cpp` (transitively pulls `ggml`)
- Models stored as `.gguf` files (single file: weights + vocab + metadata)
- **Default model**: `all-MiniLM-L6-v2.Q8_0.gguf` — ~25MB, 384-dim, good quality, fast on CPU
- **Upgrade model**: any BERT-family GGUF (e.g. `bge-small-en`, `nomic-embed-text` GGUF builds)
- Runs in a background thread at lower priority than FTS indexing
- Opt-in per workspace; disabled by default
- Tokenisation uses the WordPiece vocabulary embedded inside the GGUF file — no separate `vocab.txt` needed

**Why not ONNX Runtime?** The vcpkg `onnxruntime` port under `x64-windows-static` strips
ONNX schema-registration static constructors at link time, causing runtime "invalid model"
failures in both Debug and Release. `/WHOLEARCHIVE:ONNX::onnx` did not resolve it. llama.cpp
has no equivalent schema registry and links cleanly under static CRT. As a bonus, llama.cpp
ships a real tokenizer, replacing the stub FNV-hash tokenizer that the earlier ONNX path used.

See [workspace-index.md](workspace-index.md) for chunking strategy and hybrid search design.

---

## 6. Code Parser

**Tree-sitter** — the C library, linked directly.

- 100+ languages, incremental, error-tolerant
- C API — zero FFI overhead from C++
- Grammars packaged separately per language (vcpkg or bundled)
- Used by GitHub, Neovim, Helix, Zed — battle-tested at scale

---

## 7. File Watcher

**efsw** (Easy File System Watcher).

- Thin C++ wrapper over OS-native APIs:
  - Windows: `ReadDirectoryChangesW`
  - Linux (future): `inotify`
  - macOS (future): `FSEvents`
- Cross-platform by design — correct abstraction for future portability
- vcpkg: `efsw` package, MIT licensed

Using efsw instead of calling `ReadDirectoryChangesW` directly keeps the watcher
behind one clean interface now, making future cross-platform support a drop-in.

---

## 8. LLM API Client

**cpr + manual SSE parsing** pointed at LM Studio (`http://localhost:1234/v1`).

- `cpr` wraps libcurl with a clean C++ API, vcpkg: `cpr`
- Streaming: Server-Sent Events (SSE) parsed from the response body
- LM Studio exposes OpenAI-compatible REST — same protocol works with real
  OpenAI/Claude endpoints by changing the base URL (future backend option)

---

## 9. API Server (Core ↔ Remote Frontends)

**Crow** — lightweight C++ HTTP + WebSocket microframework.

- WebSocket built-in — required for streaming LLM tokens + tool approval flow
- HTTP REST for stateless operations (workspace mgmt, sessions, settings)
- MIT licensed, vcpkg: `crowcpp-crow`

`CrowFrontend` is the third C++ frontend — it implements `IFrontend`, registers
with Core directly, and exposes the session to external clients (browser, VS Code
extension, mobile) via HTTP/WebSocket. The CLI and wxWidgets frontends bypass Crow
entirely (direct C++ interface).

---

## 10. ZIM File Support

**libzim** — official Kiwix C++ library for reading `.zim` archives.

- Random access to articles by title or path, MIT licensed
- vcpkg: `libzim`
- Handles full English Wikipedia (~90GB ZIM) without extraction

```cpp
zim::Archive archive("wikipedia.zim");
auto entry = archive.getEntryByPath("A/Byzantine_fault");
auto item  = entry.getItem();
std::string html = item.getData().data();
```

---

## 11. Document Text Extraction

**PDF**: pdfium (Google Chrome's PDF engine, C API)
- Handles complex layouts, embedded fonts, detects encrypted files
- vcpkg: `pdfium` (~30MB)
- Fallback: poppler (lighter, LGPL) if pdfium proves too heavy

**DOCX / XLSX**: miniz + pugixml (both header-only)
- `.docx` / `.xlsx` are ZIP archives — miniz decompresses, pugixml parses inner XML
- vcpkg: `miniz`, `pugixml`

---

## 12. JSON

**nlohmann/json** — header-only, clean C++ API, vcpkg: `nlohmann-json`.
Used for API protocol messages, config files, LLM request/response parsing.
If JSON parsing ever becomes a bottleneck (unlikely): swap hot paths to simdjson.

---

## 13. Logging

**spdlog** — fast, header-only, MIT licensed, vcpkg: `spdlog`.

- Multiple sinks: rotating file log in `.locus/locus.log` + stderr for CLI/debug
- Structured levels: trace / debug / info / warn / error / critical
- Pattern includes timestamp, thread id, source location
- The Core (daemon) logs to file; the CLI prototype logs to stderr

---

## 14. Threading Model

**`std::thread` + `std::mutex` / `std::queue`** — explicit, manual, no framework.

Fits the project philosophy: direct control, no hidden runtime, predictable behaviour.

Thread layout:
| Thread | Responsibility |
|---|---|
| Main | wxWidgets event loop (UI thread) — never blocks |
| Agent | LLM request, SSE streaming, tool dispatch |
| Indexer (FTS) | File traversal + FTS5 index build/update |
| Indexer (Vec) | llama.cpp embedding, sqlite-vec writes (lower priority) |
| Watcher | efsw event loop, posts file change events to Indexer queue |
| Crow | CrowFrontend HTTP + WebSocket server for external clients |

Communication between threads via `std::queue<Event>` with `std::mutex` + `std::condition_variable`.
No lock-free structures unless profiling shows a bottleneck.

---

## 15. Test Framework

**Catch2** — clean syntax, good C++ integration, MIT licensed, vcpkg: `catch2`.

- Unit tests for: index subsystem, hybrid search ranking, tool protocol, context manager
- CLI prototype ships with tests from day one — not bolted on later
- Test binary is a separate CMake target, not linked into the main binary

---

## 16. Web Retrieval (RAG)

**HTML parser: gumbo-parser** (Google, Apache 2.0, vcpkg: `gumbo`)

- Pure C HTML5 parser — handles malformed real-world HTML correctly
- Produces a DOM tree; Locus walks it to extract visible text, skip script/style/nav
- Tiny footprint, no dependencies beyond libc
- Used for: converting fetched web pages to clean plain text for FTS5 indexing

**Search API: configurable, Brave Search default**

- Brave Search API: clean JSON REST, free tier (2,000 queries/month)
- Alternative: SearXNG (self-hosted metasearch, no API key, no limits)
- API endpoint is configurable — any provider returning the expected JSON shape works
- HTTP calls via cpr (already a dependency)

**Architecture**: fetched web pages are indexed in separate FTS5 tables (`web_fts`),
not injected into LLM context. The agent queries web content through the same
search tools used for local files. Full pages never enter context — only ranked snippets.

See [web-retrieval.md](web-retrieval.md) for the full pipeline design.

---

## Full Stack at a Glance

| Layer | Choice | vcpkg package |
|---|---|---|
| Language | C++20 | — |
| Build | CMake + vcpkg | — |
| Frontend: CLI | Terminal REPL | — |
| Frontend: wxWidgets | wxWidgets + wxWebView | `wxwidgets`, `md4c` |
| Frontend: CrowServer | Crow HTTP/WS for external clients | `crowcpp-crow` |
| Index database | SQLite + FTS5 | `sqlite3` |
| Vector search | sqlite-vec | (bundled extension) |
| Embeddings | llama.cpp (GGUF) | `llama-cpp` |
| Code parser | Tree-sitter | `tree-sitter` |
| File watcher | efsw | `efsw` |
| LLM HTTP client | cpr | `cpr` |
| HTML parser | gumbo-parser | `gumbo` |
| Web search API | Brave Search (configurable) | — |
| ZIM reader | libzim | `libzim` |
| PDF extraction | pdfium | `pdfium` |
| DOCX/XLSX | miniz + pugixml | `miniz`, `pugixml` |
| JSON | nlohmann/json | `nlohmann-json` |
| Logging | spdlog | `spdlog` |
| Threading | std::thread + std::mutex/queue | — |
| Tests | Catch2 | `catch2` |
