#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace locus {

namespace fs = std::filesystem;

// Cross-encoder reranker (e.g. bge-reranker-v2-m3) loaded as a GGUF via
// llama.cpp with LLAMA_POOLING_TYPE_RANK. For each (query, passage) pair
// the model emits a single relevance logit; higher = more relevant.
//
// Used as a second-stage refinement on top of bi-encoder embedding search:
// the embedder returns the top-K candidates by cosine, then this class
// re-scores them against the original query and picks the top-N by
// reranker score.
//
// Thread safety: rerank() takes an internal mutex; safe to call from
// multiple threads but they will serialise on the llama_context KV cache.
class Reranker {
public:
    explicit Reranker(const fs::path& model_path, int n_ctx = 1024);
    ~Reranker();

    Reranker(const Reranker&) = delete;
    Reranker& operator=(const Reranker&) = delete;

    // Score one (query, passage) pair. Returns the raw classifier logit;
    // higher = more relevant. Score scale is model-specific - only use the
    // ordering, not the absolute value.
    float score(const std::string& query, const std::string& passage) const;

    // Convenience: score `passages` against `query` and return one float per
    // passage in input order.
    std::vector<float> score_batch(const std::string& query,
                                   const std::vector<std::string>& passages) const;

    const fs::path& model_path() const { return model_path_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    fs::path model_path_;
};

} // namespace locus
