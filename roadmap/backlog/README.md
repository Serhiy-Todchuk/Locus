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
- [S4.U -- Subagents](S4.U-subagents.md). Originally an M4 stage; parked because subagents are a cloud-agent pattern that doesn't survive a single-GPU local-LLM runtime. Permanent ~250-token manifest tax, contradicts the user-as-architect / transparency thesis, no real parallelism on one GPU, and "summary not transcript" is just compaction with extra steps. Reactivate only when Locus gains real parallel inference (multi-GPU, batched vLLM, cloud fallback) or a measured workload proves an inline `search_semantic` + `read_file` loop is materially worse than a child agent.
- [S4.Q -- Multi-Model Split (Weak + Strong)](S4.Q-multi-model.md). Originally an M4 stage; parked because the Aider cost-tier rationale doesn't translate to consumer GPUs (loading two model files in finite VRAM crashes; swapping eats wall-clock). The skeleton already shipped with [S3.B](../M3/S3.B-llm-client-split.md). The version that *would* land cleanly is "CPU-side weak model for non-latency-critical jobs" -- a different stage with a different rationale, captured in the parked spec. Reactivate by rewriting around CPU offload, not Aider's cost-tier story.
- [S4.H -- Parallel Tool Calls](S4.H-parallel-tools.md). Originally an M4 stage; parked because local 7B-26B models rarely emit batched `tool_calls` (the precondition rarely fires) and per-tool latency is dwarfed by LLM round-trip time anyway -- best-case saving is sub-second per turn. The auto-approve-only carve-out kills the user-perceived benefit. Reactivate when local models reliably emit parallel calls AND tool latency stops being a rounding error.

## Items

- Tool-result caching (dedup identical tool calls in a session). Originally an S4.V bullet; dropped because it solves a problem that doesn't exist on local hardware -- a repeat `search_symbols("foo")` is sub-100 ms and agents rarely make exact-duplicate calls in practice. Engineering cost (cache key including watcher state, eviction) is real for invisible savings. Reactivate if telemetry surfaces a real workload where tool-time dominates.
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
