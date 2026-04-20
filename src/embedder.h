#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace locus {

namespace fs = std::filesystem;

// llama.cpp-based text embedder.  Loads a sentence-transformer model in
// GGUF format (e.g. all-MiniLM-L6-v2) and produces normalised float32
// embeddings.  Tokenisation uses the WordPiece/SentencePiece vocabulary
// embedded inside the GGUF file.
//
// Thread safety: embed() is safe to call from multiple threads.  The
// implementation serialises access to the underlying llama_context
// internally, because the KV cache is reset on each call.
class Embedder {
public:
    // Loads the GGUF model from disk.  Throws std::runtime_error on failure.
    explicit Embedder(const fs::path& model_path, int dimensions = 384);
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
    int dimensions_;
};

} // namespace locus
