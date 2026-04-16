#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace locus {

namespace fs = std::filesystem;

// ONNX Runtime-based text embedder.  Loads a sentence-transformer model
// (e.g. all-MiniLM-L6-v2) and produces normalised float32 embeddings.
//
// Thread safety: embed() is safe to call from multiple threads — the
// ONNX Runtime session handles concurrent inference internally.
class Embedder {
public:
    // Loads the ONNX model from disk.  Throws std::runtime_error on failure.
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
