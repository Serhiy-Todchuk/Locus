#include "embedder.h"

#include <spdlog/spdlog.h>
#include <llama.h>

#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace locus {

// ---------------------------------------------------------------------------
// Embedder::Impl  —  wraps llama.cpp session for embedding extraction
// ---------------------------------------------------------------------------
//
// llama.cpp replaces ONNX Runtime for embedding inference:
//   - Vocabulary is embedded inside the GGUF file; llama_tokenize produces
//     real WordPiece / SentencePiece IDs instead of hashed fake tokens.
//   - No static-constructor schema-registration issue that plagued
//     onnxruntime + vcpkg x64-windows-static.
//   - Mean-pooling is performed inside the context via
//     LLAMA_POOLING_TYPE_MEAN, so llama_get_embeddings_seq() already returns
//     the pooled sentence vector.  We L2-normalise afterwards to preserve
//     cosine-as-dot-product behaviour in IndexQuery.

static void init_llama_backend_once()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        llama_backend_init();
        spdlog::info("llama.cpp backend initialised");
    });
}

struct Embedder::Impl {
    llama_model*       model  = nullptr;
    llama_context*     ctx    = nullptr;
    int                n_embd = 0;
    mutable std::mutex mu;  // embed() serialises access to ctx / KV cache

    ~Impl()
    {
        if (ctx)   llama_free(ctx);
        if (model) llama_model_free(model);
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Embedder::Embedder(const fs::path& model_path, int dimensions)
    : model_path_(model_path)
    , dimensions_(dimensions)
{
    if (!fs::exists(model_path)) {
        throw std::runtime_error("Embedding model not found: " + model_path.string());
    }

    init_llama_backend_once();

    impl_ = std::make_unique<Impl>();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;      // CPU-only; desktop app, no GPU assumed
    mparams.use_mmap     = true;

    impl_->model = llama_model_load_from_file(model_path.string().c_str(), mparams);
    if (!impl_->model) {
        throw std::runtime_error("llama_model_load_from_file failed: " + model_path.string());
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.embeddings   = true;
    cparams.pooling_type = LLAMA_POOLING_TYPE_MEAN;
    cparams.n_ctx        = 512;
    cparams.n_batch      = 512;
    cparams.n_ubatch     = 512;

    impl_->ctx = llama_init_from_model(impl_->model, cparams);
    if (!impl_->ctx) {
        llama_model_free(impl_->model);
        impl_->model = nullptr;
        throw std::runtime_error("llama_init_from_model failed");
    }

    impl_->n_embd = llama_model_n_embd(impl_->model);
    if (impl_->n_embd != dimensions_) {
        spdlog::warn("Model n_embd={} does not match requested dimensions={}; "
                     "output will be padded/truncated",
                     impl_->n_embd, dimensions_);
    }

    spdlog::info("Embedder loaded: {} (n_embd={})", model_path.string(), impl_->n_embd);
}

Embedder::~Embedder() = default;

std::vector<float> Embedder::embed(const std::string& text) const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    const llama_vocab* vocab = llama_model_get_vocab(impl_->model);

    // Tokenize.  First call with a reasonably-sized buffer; if the result is
    // negative, it tells us the required capacity, so we retry once.
    std::vector<llama_token> tokens(512);
    int32_t n = llama_tokenize(
        vocab,
        text.data(), static_cast<int32_t>(text.size()),
        tokens.data(), static_cast<int32_t>(tokens.size()),
        /*add_special*/  true,
        /*parse_special*/ false);
    if (n < 0) {
        tokens.resize(static_cast<size_t>(-n));
        n = llama_tokenize(
            vocab,
            text.data(), static_cast<int32_t>(text.size()),
            tokens.data(), static_cast<int32_t>(tokens.size()),
            true, false);
        if (n < 0) {
            throw std::runtime_error("llama_tokenize failed");
        }
    }
    tokens.resize(static_cast<size_t>(n));

    // Empty input: llama_decode rejects zero-token batches.  Return a zero
    // vector (consistent with the previous behaviour for empty text).
    if (tokens.empty()) {
        return std::vector<float>(dimensions_, 0.0f);
    }

    // Clamp to context length.
    const uint32_t n_ctx = llama_n_ctx(impl_->ctx);
    if (tokens.size() > n_ctx) {
        tokens.resize(n_ctx);
    }

    // Build a single-sequence batch.
    llama_batch batch = llama_batch_init(static_cast<int32_t>(tokens.size()), 0, 1);
    for (int32_t i = 0; i < static_cast<int32_t>(tokens.size()); ++i) {
        batch.token[i]     = tokens[i];
        batch.pos[i]       = i;
        batch.n_seq_id[i]  = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i]    = true;   // for pooled embeddings, mark tokens as outputs
    }
    batch.n_tokens = static_cast<int32_t>(tokens.size());

    // Clear KV cache before each independent call so sequence 0 is reusable.
    llama_memory_clear(llama_get_memory(impl_->ctx), /*data*/ true);

    // Encoder-only embedding models (BERT-style) use llama_encode; calling
    // llama_decode triggers a fallback warning on every call.
    if (llama_encode(impl_->ctx, batch) != 0) {
        llama_batch_free(batch);
        throw std::runtime_error("llama_encode failed");
    }

    const float* pooled = llama_get_embeddings_seq(impl_->ctx, 0);
    llama_batch_free(batch);

    if (!pooled) {
        throw std::runtime_error("llama_get_embeddings_seq returned null");
    }

    std::vector<float> out(pooled, pooled + impl_->n_embd);

    // L2 normalise — pooling_type=MEAN does not normalise; downstream
    // IndexQuery::search_semantic treats cosine distance as 1 - dot product,
    // which requires unit-length vectors.
    float norm = 0.0f;
    for (float v : out) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (auto& v : out) v /= norm;
    }

    out.resize(dimensions_, 0.0f);
    return out;
}

} // namespace locus
