# Technology Stack

> **Chosen stack: C++20 + CMake + vcpkg.** Starting with CLI prototype, desktop UI TBD.
> User profile: 15+ years C++/C#/shaders (3D graphics). Performance-first.
> Hard requirement: NO Electron. App must be lean, responsive, low memory.
> Starting platform: Windows 11.

---

## The Core Decision: C++ vs Rust

This is the foundational choice that determines everything else.

### Option A — C++ + Dear ImGui

**C++ is fully viable for Locus.** All key libraries are available:
- **HTTP/LLM API**: [cpr](https://github.com/libcpr/cpr) (libcurl wrapper) or WinHTTP directly — clean C++ HTTP with streaming support
- **JSON**: [nlohmann/json](https://github.com/nlohmann/nlohmann_json) — header-only, excellent C++ JSON
- **SQLite**: Native C library — C++ wrappers (SQLiteCpp or raw API) work great
- **Tree-sitter**: C library — perfect native C++ integration, no FFI overhead
- **File watcher**: ReadDirectoryChangesW (Win32) or [efsw](https://github.com/SpartanJ/efsw) cross-platform
- **Package manager**: vcpkg or Conan

**UI — Dear ImGui:**
- User almost certainly knows ImGui from graphics work
- Immediate mode = zero retained state overhead, perfect for a tool app
- Rendering backend: DX11 or DX12 (natural choice for this user)
- Markdown rendering: [imgui_md](https://github.com/mekhontsev/imgui_md) library
- Code editor/viewer with syntax highlight: [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit)
- Streaming text output: trivial (append to a `std::string` buffer, ImGui renders on next frame)
- File tree: straightforward with ImGui TreeNode
- **Result**: a ~2MB native binary with zero external runtime

**Pros:**
- User's native language — likely fastest iteration for them
- Zero runtime overhead — binary is ~2MB, startup is instant
- Full control over rendering pipeline and memory
- All libraries used (SQLite, Tree-sitter) are C — natural fit
- Easy DX11/DX12 backend: something the user can optimize if needed

**Cons:**
- More boilerplate for async operations (LLM streaming + UI responsiveness)
- No package ecosystem as cohesive as Cargo
- Memory management is manual (mitigated by user experience)
- Async streaming from HTTP in C++ requires care (threads or async IO)

---

### Option B — Rust + Tauri

**Rust backend + WebView2 frontend (HTML/CSS/JS via OS-provided Edge Chromium component)**

- WebView2 is part of Windows 11 / redistributable — NOT bundled Chromium, shared OS component
- App bundle: ~5MB Rust binary + WebView2 (already on machine)
- Memory: much lower than Electron; close to native
- UI: HTML/CSS/JS in WebView2 — markdown, code highlighting, streaming all "free"
- Backend: Rust (Tauri) handles filesystem, index building, file watching, LLM calls

**Pros:**
- Memory safety without GC overhead
- Cargo ecosystem: excellent libraries for everything needed
- WebView2 on Windows = effectively zero UI overhead (shared with Edge/VS Code/Teams)
- Frontend: rich web UI without bundling a browser
- Tree-sitter has Rust bindings, SQLite (rusqlite) is solid
- Async (tokio) is excellent for LLM streaming + file ops
- Natural path to VS Code extension (TypeScript frontend, Rust WASM modules)

**Cons:**
- Rust learning curve (though syntax is learnable for an experienced C++ dev)
- WebView2 = web rendering stack, not immediate mode — different mental model
- Frontend JS/TS adds a second language to the project
- IPC between Rust backend and JS frontend (Tauri commands) adds a layer

---

### Recommendation

**Both are real options.** The decision hinges on user preference:

| Factor | C++ + ImGui | Rust + Tauri |
|---|---|---|
| UI feel | Immediate mode, pixel-perfect control | HTML/CSS, richer out of box |
| Memory | Lowest possible | Very low (WebView2 shared) |
| Dev speed | Fast for this user (native language) | Moderate (Rust learning curve) |
| Ecosystem | Good (vcpkg, mature libs) | Excellent (Cargo) |
| Streaming UI | Manual (thread + lock or async) | Easy (JS EventSource / SSE) |
| Future web UI | Hard | Easy (same frontend code) |
| VS Code ext path | Very hard | Natural (TypeScript frontend) |

**If the priority is max performance + user comfort**: C++ + ImGui.
**If the priority is future extensibility + safer memory**: Rust + Tauri.

_Decision pending user input._

---

## 2. C++ UI Options (if C++ is chosen)

### Dear ImGui + DX11/DX12 backend
- **Recommended for this user** — immediate mode, graphics-native, zero overhead
- Rendering: DX11 is simplest; DX12 for anyone who wants GPU-driven UI tricks
- imgui_md for markdown, ImGuiColorTextEdit for code view
- Build: CMake + vcpkg

### Sciter
- Embeds an HTML/CSS/JS rendering engine in a C++ app as a DLL (~10MB)
- Write UI in HTML/CSS, call C++ backend from JS
- Web-style UI with C++ backend — similar concept to Tauri but in C++
- License: free for open-source, paid for commercial
- [sciter.com](https://sciter.com)

### WinUI 3 (Windows App SDK)
- Microsoft's modern native Windows UI framework (C++/WinRT or C#)
- Native Windows look and feel, excellent accessibility
- Good for a Windows-first app
- Con: Windows-only; heavier API than ImGui; C++/WinRT is verbose

### Qt 6
- Mature, cross-platform, excellent for complex GUIs
- QML for declarative UI — reasonably fast to write
- Con: heavy (~50MB runtime), LGPL license complexity, not lean by this user's standards

**If C++ is chosen: Dear ImGui is the recommendation for v1 prototyping.**

---

## 3. Primary Backend Language (resolved by Option A/B above)

If C++: C++20 with CMake + vcpkg.
If Rust: Rust stable + Cargo + Tauri.

---

## 4. Index Database

### SQLite + FTS5
- **Recommended regardless of language choice**
- C API works perfectly from both C++ and Rust
- FTS5 gives BM25-ranked full-text search
- `sqlite-vec` extension adds vector search when needed
- Single file, zero server, zero config

### Tantivy (Rust-only)
- Rust-native full-text search engine, extremely fast
- Only available if Rust is chosen
- Overkill for v1, but a future upgrade path

**Decision: SQLite + FTS5 for v1, regardless of language.**

---

## 5. Code Parser

### Tree-sitter
- C library — integrates natively from both C++ and Rust
- Incremental, error-tolerant, 100+ languages
- Battle-tested (GitHub, Neovim, Helix, Zed)
- Rust: `tree-sitter` crate. C++: link the C library directly.
- **Recommended.**

---

## 6. File Watcher

### Windows: ReadDirectoryChangesW (C++ option)
- Win32 API — zero dependency, very efficient, OS-native
- Works for a Windows-first app

### efsw (C++ cross-platform)
- Thin C++ wrapper over OS file watching APIs
- If cross-platform is needed eventually

### Rust notify crate (Rust option)
- Cross-platform, OS-native events (FSEvents/inotify/ReadDirectoryChangesW)
- Excellent with tokio async

**Decision: OS-native in both cases. ReadDirectoryChangesW for C++, notify crate for Rust.**

---

## 7. LLM API Client

LM Studio exposes an OpenAI-compatible REST API at `http://localhost:1234/v1`.

### C++: cpr + nlohmann/json + streaming
- `cpr` wraps libcurl with clean C++ API
- Server-Sent Events (SSE) for streaming responses
- Straightforward but requires manual SSE parsing

### Rust: reqwest + tokio + async streams
- `reqwest` with streaming body, tokio async
- Clean async/await streaming
- `async-openai` crate wraps the protocol

**Both work. Rust async is somewhat cleaner for streaming.**

---

## 8. Embedding / Semantic Search (Deferred — v2)

- When needed: run a small embedding model via LM Studio embedding API
- Or: onnxruntime (has both C++ and Rust bindings) for in-process inference
- sqlite-vec extension for vector storage alongside FTS5

**Defer to v2.**

---

## Summary Table (Two Stack Options)

| Layer | C++ Stack | Rust/Tauri Stack |
|---|---|---|
| App shell | CMake binary + DX11 | Tauri (Rust + WebView2) |
| Backend language | C++20 | Rust (stable) |
| UI framework | Dear ImGui | HTML/CSS/JS (Svelte) in WebView2 |
| Database | SQLite C API + FTS5 | rusqlite + FTS5 |
| Code parser | Tree-sitter (C lib) | tree-sitter crate |
| File watcher | ReadDirectoryChangesW | notify crate |
| LLM client | cpr + SSE parsing | async-openai / reqwest |
| Package manager | vcpkg | Cargo |
| Embedding (v2) | onnxruntime C++ | ort crate (onnxruntime) |

Both stacks are viable. Choose based on language comfort and UI goals.
