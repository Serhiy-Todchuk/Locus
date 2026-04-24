#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace locus {

namespace fs = std::filesystem;

// llama.cpp-based text embedder.  Loads a sentence-transformer model in
// GGUF format (e.g. bge-m3, bge-small-en-v1.5) and produces L2-normalised
// float32 embeddings.
//
// Embedding dimension is read from the GGUF (llama_model_n_embd); callers
// must not assume a fixed size.  n_ctx defaults to 1024 — large enough for
// a typical 80-line code chunk, small enough to keep CPU embed latency
// reasonable on commodity hardware.  bge-m3 supports up to 8192 if a future
// caller needs it.
//
// Thread safety: embed() is safe to call from multiple threads.  The
// implementation serialises access to the underlying llama_context
// internally, because the KV cache is reset on each call.
class Embedder {
public:
    // Loads the GGUF model from disk.  Throws std::runtime_error on failure.
    explicit Embedder(const fs::path& model_path, int n_ctx = 1024);
    ~Embedder();

    Embedder(const Embedder&) = delete;
    Embedder& operator=(const Embedder&) = delete;

    // Embed a single text string.  Returns a normalised vector of size
    // dimensions().
    std::vector<float> embed(const std::string& text) const;

    int dimensions() const { return dimensions_; }
    const fs::path& model_path() const { return model_path_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    fs::path model_path_;
    int dimensions_ = 0;  // populated from llama_model_n_embd in ctor
};

} // namespace locus
