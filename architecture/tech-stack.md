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

## 2. Frontend — v1: Dear ImGui + DX11

**Decided for v1** — deferred until after CLI prototype.

- Immediate mode UI — zero retained state, perfect for a tool app
- DX11 backend: native Windows, user's home territory from graphics work
- Markdown rendering: [imgui_md](https://github.com/mekhontsev/imgui_md)
- Code view with syntax highlight: [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit)
- Streaming LLM output: append to `std::string` buffer, ImGui renders on next frame
- Result: ~2–4MB binary, instant startup, no external runtime

ImGui connects to Core via the **C++ direct interface** (`IFrontend` / `ILocusCore`
virtual calls, same process, zero overhead). See [overview.md](overview.md).

---

## 3. Future Frontend Options

Core exposes a frontend-agnostic API (C++ direct + WebSocket/HTTP). Any of the
following can be attached later without modifying Core. Multiple can run simultaneously.

### Rust + Tauri (web frontend on C++ core)
- Tauri app connects to the Core's WebSocket/HTTP API like any other remote frontend
- Frontend: HTML/CSS/JS (Svelte) in WebView2 — markdown, code highlight, streaming free
- C++ Core remains unchanged — Tauri is just another API client
- Benefit: rich web UI, natural path to browser-based remote access
- Note: Rust is only used for the thin Tauri shell; Core stays C++

### Sciter (HTML/CSS in C++ process)
- Embeds an HTML/CSS/JS renderer as a DLL in the C++ app (~10MB)
- UI written in HTML/CSS, calls C++ backend directly
- Web-style richness with zero separate process
- License: free for open-source
- [sciter.com](https://sciter.com)

### WinUI 3 (Windows App SDK)
- Microsoft's modern native Windows UI (C++/WinRT)
- Best Windows integration (accessibility, dark mode, system fonts)
- Windows-only; C++/WinRT is verbose but well-documented

### Browser / PWA
- Any browser connecting to Core's HTTP/WebSocket server
- Zero frontend deployment — user opens `http://localhost:PORT`
- Progressive Web App: installable on desktop or mobile from browser
- Natural path for the mobile remote access use case

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

**ONNX Runtime C++ API** — in-process, no dependency on LM Studio being up.

- Official ONNX Runtime C++ API, Apache 2.0, vcpkg: `onnxruntime`
- Models stored in `.locus/models/` as `.onnx` files
- **Default model**: `all-MiniLM-L6-v2` — 22MB, 384-dim, good quality, fast on CPU
- **Upgrade model**: `nomic-embed-text-v1.5` — 280MB, 768-dim, better for long docs
- Runs in a background thread at lower priority than FTS indexing
- Opt-in per workspace; disabled by default

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

The C++ ImGui frontend bypasses Crow entirely (direct C++ interface).
Crow serves only remote frontends (Tauri, browser, mobile).

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
| Main | ImGui render loop (UI thread) — never blocks |
| Agent | LLM request, SSE streaming, tool dispatch |
| Indexer (FTS) | File traversal + FTS5 index build/update |
| Indexer (Vec) | ONNX embedding, sqlite-vec writes (lower priority) |
| Watcher | efsw event loop, posts file change events to Indexer queue |
| Crow | HTTP + WebSocket server for remote frontends |

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
| Frontend v1 | Dear ImGui + DX11 | `imgui` |
| Future frontends | Tauri/web, Sciter, WinUI3, browser PWA | — |
| API server (remote) | Crow | `crowcpp-crow` |
| Index database | SQLite + FTS5 | `sqlite3` |
| Vector search | sqlite-vec | (bundled extension) |
| Embeddings | ONNX Runtime | `onnxruntime` |
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
