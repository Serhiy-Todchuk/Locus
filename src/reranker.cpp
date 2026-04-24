#include "reranker.h"

#include <spdlog/spdlog.h>
#include <llama.h>

#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace locus {

// ---------------------------------------------------------------------------
// Reranker - bge-reranker-v2-m3 style cross-encoder via llama.cpp
// ---------------------------------------------------------------------------
//
// Input format for the BGE reranker family is:
//   <BOS> query_tokens <SEP> passage_tokens <EOS>
// We tokenize query and passage separately (with add_special=false so we
// control the boundary tokens), then assemble. llama_get_embeddings_seq
// returns float[n_cls_out] when pooling_type == LLAMA_POOLING_TYPE_RANK;
// for bge-reranker-v2-m3, n_cls_out == 1.

static void init_llama_backend_once_rr()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        llama_backend_init();
        spdlog::info("llama.cpp backend initialised (reranker)");
    });
}

struct Reranker::Impl {
    llama_model*       model     = nullptr;
    llama_context*     ctx       = nullptr;
    int                n_cls_out = 1;
    int                n_ctx     = 0;
    llama_token        bos       = -1;
    llama_token        eos       = -1;
    llama_token        sep       = -1;
    mutable std::mutex mu;

    ~Impl()
    {
        if (ctx)   llama_free(ctx);
        if (model) llama_model_free(model);
    }
};

Reranker::Reranker(const fs::path& model_path, int n_ctx)
    : model_path_(model_path)
{
    if (!fs::exists(model_path)) {
        throw std::runtime_error("Reranker model not found: " + model_path.string());
    }

    init_llama_backend_once_rr();
    impl_ = std::make_unique<Impl>();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    mparams.use_mmap     = true;

    impl_->model = llama_model_load_from_file(model_path.string().c_str(), mparams);
    if (!impl_->model) {
        throw std::runtime_error("llama_model_load_from_file failed: " + model_path.string());
    }

    int trained_ctx = static_cast<int>(llama_model_n_ctx_train(impl_->model));
    if (trained_ctx > 0 && n_ctx > trained_ctx) n_ctx = trained_ctx;
    if (n_ctx <= 0) n_ctx = 512;
    impl_->n_ctx = n_ctx;

    llama_context_params cparams = llama_context_default_params();
    cparams.embeddings   = true;
    cparams.pooling_type = LLAMA_POOLING_TYPE_RANK;
    cparams.n_ctx        = static_cast<uint32_t>(n_ctx);
    cparams.n_batch      = static_cast<uint32_t>(n_ctx);
    cparams.n_ubatch     = static_cast<uint32_t>(n_ctx);

    impl_->ctx = llama_init_from_model(impl_->model, cparams);
    if (!impl_->ctx) {
        llama_model_free(impl_->model);
        impl_->model = nullptr;
        throw std::runtime_error("llama_init_from_model failed (reranker)");
    }

    impl_->n_cls_out = static_cast<int>(llama_model_n_cls_out(impl_->model));
    if (impl_->n_cls_out < 1) impl_->n_cls_out = 1;

    const llama_vocab* vocab = llama_model_get_vocab(impl_->model);
    impl_->bos = llama_vocab_bos(vocab);
    impl_->eos = llama_vocab_eos(vocab);
    impl_->sep = llama_vocab_sep(vocab);

    spdlog::info("Reranker loaded: {} (n_cls_out={}, n_ctx={}, bos={}, eos={}, sep={})",
                 model_path.string(), impl_->n_cls_out, n_ctx,
                 impl_->bos, impl_->eos, impl_->sep);
}

Reranker::~Reranker() = default;

namespace {

// Tokenise without special tokens; returns the token ids.
std::vector<llama_token> tokenize_plain(const llama_vocab* vocab,
                                        const std::string& text)
{
    if (text.empty()) return {};
    std::vector<llama_token> tokens(text.size() + 16);
    int32_t n = llama_tokenize(
        vocab, text.data(), static_cast<int32_t>(text.size()),
        tokens.data(), static_cast<int32_t>(tokens.size()),
        /*add_special*/ false, /*parse_special*/ false);
    if (n < 0) {
        tokens.resize(static_cast<size_t>(-n));
        n = llama_tokenize(
            vocab, text.data(), static_cast<int32_t>(text.size()),
            tokens.data(), static_cast<int32_t>(tokens.size()),
            false, false);
        if (n < 0) {
            throw std::runtime_error("llama_tokenize failed (reranker)");
        }
    }
    tokens.resize(static_cast<size_t>(n));
    return tokens;
}

} // namespace

float Reranker::score(const std::string& query, const std::string& passage) const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    const llama_vocab* vocab = llama_model_get_vocab(impl_->model);

    auto qtok = tokenize_plain(vocab, query);
    auto ptok = tokenize_plain(vocab, passage);

    // Reserve room for BOS + SEP + EOS framing.
    const int reserved = 3;
    int budget = impl_->n_ctx - reserved;
    if (budget <= 0) budget = 16;

    // Cap query at 1/4 of the budget to leave room for the passage.
    int q_cap = std::max(8, budget / 4);
    if (static_cast<int>(qtok.size()) > q_cap) qtok.resize(static_cast<size_t>(q_cap));

    int p_cap = budget - static_cast<int>(qtok.size());
    if (p_cap < 0) p_cap = 0;
    if (static_cast<int>(ptok.size()) > p_cap) ptok.resize(static_cast<size_t>(p_cap));

    std::vector<llama_token> seq;
    seq.reserve(qtok.size() + ptok.size() + reserved);
    if (impl_->bos >= 0) seq.push_back(impl_->bos);
    seq.insert(seq.end(), qtok.begin(), qtok.end());
    if (impl_->sep >= 0) seq.push_back(impl_->sep);
    seq.insert(seq.end(), ptok.begin(), ptok.end());
    if (impl_->eos >= 0) seq.push_back(impl_->eos);

    if (seq.empty()) return 0.0f;

    llama_batch batch = llama_batch_init(static_cast<int32_t>(seq.size()), 0, 1);
    for (int32_t i = 0; i < static_cast<int32_t>(seq.size()); ++i) {
        batch.token[i]     = seq[i];
        batch.pos[i]       = i;
        batch.n_seq_id[i]  = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i]    = true;
    }
    batch.n_tokens = static_cast<int32_t>(seq.size());

    llama_memory_clear(llama_get_memory(impl_->ctx), /*data*/ true);

    if (llama_encode(impl_->ctx, batch) != 0) {
        llama_batch_free(batch);
        throw std::runtime_error("llama_encode failed (reranker)");
    }

    const float* out = llama_get_embeddings_seq(impl_->ctx, 0);
    llama_batch_free(batch);

    if (!out) {
        throw std::runtime_error("llama_get_embeddings_seq returned null (reranker)");
    }
    return out[0];
}

std::vector<float> Reranker::score_batch(const std::string& query,
                                         const std::vector<std::string>& passages) const
{
    std::vector<float> scores;
    scores.reserve(passages.size());
    for (const auto& p : passages) {
        scores.push_back(score(query, p));
    }
    return scores;
}

} // namespace locus
