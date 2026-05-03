# Backlog

Unscheduled tasks. Items here are not part of any milestone -- they are recognized as worth doing eventually but have not been scoped, sequenced, or assigned to a milestone yet.

When an item is picked up:
- promote it into the right milestone's stage list (`roadmap/Mx/README.md`),
- give it a stage letter, and
- create the per-stage doc under `roadmap/Mx/Sx.<letter>-<slug>.md`.

Until then, items live here as one-line entries (or short paragraphs when the rationale is non-trivial). Items that already have a written-out spec live as their own files in this folder and are linked from the **Drafts** section below.

---

## Drafts (full specs, parked)

- [S4.E -- LSP Integration (via MCP)](S4.E-lsp.md). Originally an M4 stage; demoted because [S4.G -- MCP](../M4/S4.G-mcp.md) + [S4.C -- Auto-Verify](../M4/S4.C-auto-verify.md) cover the most common LSP needs (external tools and diagnostics) and a custom in-process LSP client is unjustified maintenance. Revisit if a workflow demonstrates that MCP-wrapped LSP is meaningfully worse than native (latency-bound interactive use, large-batch reference lookups). The spec already captures the rationale and the reduced-scope "curate `mcp.json` snippets + a Settings preset picker" plan if a future maintainer wants a smaller landing path.

## Items

- Native mobile app (CrowServer client).
- Voice chat.
- Feeding an image as an input to LLM.
- Online model backends (Claude, OpenAI, Gemini).
- Full VS Code extension (embedded UI panel, not just shim).
- Plugin system for community tools.
- Approve or modify tool actions.
- Line-level diff viewer for AI-proposed file changes (wxStyledTextCtrl, dtl library).
- Multiple workspaces open simultaneously (tabbed sessions).
- Export session as markdown report.
- Automated coding pipeline (plan -> code -> test -> fix loop, user-supervised).
- LaTeX math display in LLM chat (for formulas).
- Symbol index: extract member variables / field declarations (C++ `field_declaration`, etc.).
- Bundled `LlamaCppClient` as a second `ILLMClient` backend (optional). Reuses llama.cpp already linked for embeddings; gives zero-install first-run with a curated GGUF. Keeps LM Studio / Ollama / llama-server path as primary (any OpenAI-compatible endpoint). Costs: GPU-backend distribution (CPU + Vulkan realistic, CUDA adds hundreds of MB), per-model chat-template + tool-call parsing we currently get free from LM Studio, and model-management UX (picker, download, swap).
- Open-citation UX: agent cites `file:line` or `file:page` -> click opens the right thing per format. Shell-execute for PDF/DOCX/XLSX (native app), in-app render for ZIM articles + markdown + plain text (reuse existing WebView/md4c), hand off to VS Code for code files. Avoids building a universal file viewer while still letting the user verify what the agent saw. ZIM article rendering is the only hard requirement (no external viewer exists) and is implicitly needed by [S6.2](../M6/S6.2-zim-reader.md).
- Lightweight chat UI: replace `wxWebView` with `wxHtmlWindow` (optional). Keeps md4c -> HTML pipeline, drops the browser engine (tens of MB -> hundreds of KB per instance, no cold-start cost). Loses: JS-powered features (Mermaid, KaTeX, interactive code views) -- those would need WebView back for just those views.
- CLI: raw-mode line editor (vendor replxx or similar) -- suppresses visible `^[[200~` bracketed-paste markers on Windows, adds history, arrow-key editing. Current CLI uses line-mode stdin + terminal echo, which renders VT input sequences literally during paste. Non-blocking since desktop GUI is the primary frontend.
