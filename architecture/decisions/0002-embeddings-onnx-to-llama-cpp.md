# 0002. Replace ONNX Runtime with llama.cpp for embeddings

- **Status**: Accepted
- **Date**: 2026-04-09
- **Commit**: b9d39cc (`Semantic search finally works! Replaced ONNX with llama.cpp`)

## Context

Semantic search needs an in-process CPU embedder so Locus is not dependent on LM Studio being
up and so workspaces with semantic-off pay nothing. The initial choice was **ONNX Runtime** with
`all-MiniLM-L6-v2` exported to ONNX: it is the de-facto standard for portable inference and has
an existing vcpkg port.

Two problems made it unshippable under our build constraints:

1. **Static CRT + static ONNX = broken schema registry.** Our project links with the
   `x64-windows-static` vcpkg triplet (static CRT, static deps). ONNX Runtime's operator and
   graph schemas are registered via static constructors in translation units that contain no
   symbols referenced by the linker. The MSVC linker strips those TUs. At runtime the model
   load fails with "invalid model" because the schemas it references were never registered.
   Both Debug and Release were affected. `/WHOLEARCHIVE:ONNX::onnx` was attempted and did not
   resolve it — the stripping happens across multiple nested archives and the schema registry
   has no single anchor symbol to whole-archive.
2. **No tokenizer.** `all-MiniLM-L6-v2.onnx` ships without a tokenizer. We were running a
   placeholder FNV-hash tokenizer that produced technically-valid but semantically-meaningless
   token IDs — good enough to prove the pipeline wired up, nowhere near good enough to ship.

## Decision

Replace ONNX Runtime with **llama.cpp's C API** (vcpkg `llama-cpp`, transitively pulls `ggml`).

- Embedding models are distributed as single `.gguf` files (weights + vocab + metadata in one
  blob). Default model: `all-MiniLM-L6-v2.Q8_0.gguf` (~25 MB, 384-dim).
- `Embedder` in [src/embedder.h/cpp](../../src/embedder.h) loads the GGUF, tokenises via the
  WordPiece vocab baked into the file, runs inference, and returns an L2-normalised vector.
- The embedding worker runs on its own thread and writes to `vectors.db` on its own connection
  (see [ADR 0001](0001-split-index-into-main-and-vectors.md)).

## Consequences

**Wins**
- Links cleanly under static CRT — llama.cpp has no equivalent static-constructor schema
  registry. No `/WHOLEARCHIVE` games required.
- Real tokenizer. The WordPiece vocab shipped inside the GGUF produces correct token IDs; no
  more FNV stub.
- Single file per model. Swapping to `bge-small-en` or `nomic-embed-text` is one file change.
- We already planned llama.cpp as the offline-LLM fallback (future milestone). Using it for
  embeddings means one inference engine in the binary, not two.

**Costs**
- Binary size grows modestly (llama.cpp + ggml). Acceptable; still well under "bloated."
- llama.cpp's API surface is larger than ONNX's `Ort::Session::Run`. `Embedder` hides this.
- Tied to the BERT-family embedding models that llama.cpp supports. In practice this is every
  embedding model anyone would actually use on CPU.

## Alternatives considered

- **Fix the ONNX static-constructor stripping.** Would require either moving to a dynamic ONNX
  build (breaks the single-exe-plus-DLLs distribution we want) or patching the vcpkg port to
  force-reference every schema TU. High effort, fragile, and still leaves the tokenizer problem.
- **sentence-transformers via a subprocess or Python embed.** Adds a runtime dependency, defeats
  the "no hidden runtime" philosophy, and complicates shipping.
- **Call out to LM Studio's `/v1/embeddings`.** Couples semantic indexing to LM Studio being up
  and loaded with an embedding model. Unacceptable for a background indexer.
