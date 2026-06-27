Locus - semantic search models
===============================

Locus runs without any model here: full-text search, symbol search, and the
agent all work. A local embedding model is only needed for SEMANTIC search
(meaning-based retrieval) and the optional reranker improves result ordering.

Drop a GGUF embedding model into this folder, or run one of the download
scripts below. Locus looks for the file named in .locus/config.json
(index.embedding_model, default: bge-m3-Q8_0.gguf) next to the executable.

Download scripts
----------------

Recommended profile (multilingual, ~1.27 GB total):
    Windows:  powershell -ExecutionPolicy Bypass -File download.ps1
    macOS:    ./download.sh

    bge-m3-Q8_0.gguf              ~635 MB   1024-dim embedder, multilingual + code
    bge-reranker-v2-m3-Q8_0.gguf  ~636 MB   cross-encoder reranker

Small profile (English-only, ~58 MB total):
    Windows:  powershell -ExecutionPolicy Bypass -File download-small.ps1
    macOS:    ./download-small.sh

    bge-small-en-v1.5-Q8_0.gguf      ~37 MB   384-dim embedder, English
    ms-marco-MiniLM-L6-v2-Q4_K_M.gguf ~21 MB  fast English reranker

Flags (both scripts):
    -Embedder       download the embedder only
    -Force          re-download even if present
    download.ps1       -Reranker      reranker only
    download-small.ps1 -NoReranker    skip the reranker

After downloading, restart Locus (or reopen the workspace) so it picks up the
model. If the dimensions differ from a previous model, Locus re-embeds the
workspace automatically.
