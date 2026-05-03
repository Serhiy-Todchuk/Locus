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
// Embedder::Impl  -  wraps llama.cpp session for embedding extraction
// ---------------------------------------------------------------------------
//
// Pooling = MEAN. The dimension is taken from the loaded GGUF
// (llama_model_n_embd) so swapping models (MiniLM 384 -> bge-m3 1024)
// requires no config change. Output is L2-normalised so cosine similarity
// reduces to a dot product downstream in IndexQuery.

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

Embedder::Embedder(const fs::path& model_path, int n_ctx)
    : model_path_(model_path)
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

    // Cap n_ctx at what the model was trained for; some embedders (MiniLM)
    // train at 512 and asking for more is wasteful (or invalid).
    int trained_ctx = static_cast<int>(llama_model_n_ctx_train(impl_->model));
    if (trained_ctx > 0 && n_ctx > trained_ctx) n_ctx = trained_ctx;
    if (n_ctx <= 0) n_ctx = 512;

    llama_context_params cparams = llama_context_default_params();
    cparams.embeddings   = true;
    cparams.pooling_type = LLAMA_POOLING_TYPE_MEAN;
    cparams.n_ctx        = static_cast<uint32_t>(n_ctx);
    cparams.n_batch      = static_cast<uint32_t>(n_ctx);
    cparams.n_ubatch     = static_cast<uint32_t>(n_ctx);

    impl_->ctx = llama_init_from_model(impl_->model, cparams);
    if (!impl_->ctx) {
        llama_model_free(impl_->model);
        impl_->model = nullptr;
        throw std::runtime_error("llama_init_from_model failed");
    }

    impl_->n_embd = llama_model_n_embd(impl_->model);
    dimensions_   = impl_->n_embd;

    spdlog::info("Embedder loaded: {} (n_embd={}, n_ctx={})",
                 model_path.string(), impl_->n_embd, n_ctx);
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

    llama_memory_clear(llama_get_memory(impl_->ctx), /*data*/ true);

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

    float norm = 0.0f;
    for (float v : out) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (auto& v : out) v /= norm;
    }
    return out;
}

} // namespace locus
