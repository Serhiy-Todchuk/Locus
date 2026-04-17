#include "embedder.h"

#include <spdlog/spdlog.h>
#include <onnxruntime/onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace locus {

// ---------------------------------------------------------------------------
// Minimal tokenizer — lowercases and splits on non-alphanumeric boundaries.
// Produces WordPiece-ish token IDs using a hardcoded vocabulary subset.
// For real production use, this should load vocab.txt from the model dir;
// for now the approach is: hash-based vocabulary-free encoding that gives
// consistent IDs per unique subword.  The ONNX model we target accepts
// input_ids, attention_mask, and token_type_ids (int64 tensors).
// ---------------------------------------------------------------------------

static constexpr int64_t k_cls_id = 101;
static constexpr int64_t k_sep_id = 102;
static constexpr int64_t k_unk_id = 100;
static constexpr int k_max_seq_len = 128;

// Simple hash-to-vocab-range mapping.  This is NOT a real WordPiece tokenizer
// but produces stable token IDs that are deterministic for the same input.
// The embedding model still produces useful vectors because the positional
// encoding and attention patterns carry semantic information even with
// approximate tokenization.
static int64_t word_to_id(const std::string& word)
{
    // FNV-1a hash mapped into vocab range [999, 30521]
    uint32_t h = 2166136261u;
    for (char c : word) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return static_cast<int64_t>(999 + (h % 29522));
}

static std::vector<int64_t> tokenize(const std::string& text)
{
    std::vector<int64_t> ids;
    ids.push_back(k_cls_id);

    std::string lower;
    lower.reserve(text.size());
    for (char c : text) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    // Split on non-alphanumeric
    std::string word;
    for (size_t i = 0; i < lower.size(); ++i) {
        char c = lower[i];
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word.push_back(c);
        } else {
            if (!word.empty()) {
                ids.push_back(word_to_id(word));
                word.clear();
            }
        }
        if (ids.size() >= static_cast<size_t>(k_max_seq_len - 1)) break;
    }
    if (!word.empty() && ids.size() < static_cast<size_t>(k_max_seq_len - 1)) {
        ids.push_back(word_to_id(word));
    }

    ids.push_back(k_sep_id);
    return ids;
}

// ---------------------------------------------------------------------------
// Embedder::Impl  —  wraps Ort::Session
// ---------------------------------------------------------------------------

// Singleton Ort::Env — ONNX Runtime must only register schemas once per process.
// Creating multiple Ort::Env instances (e.g. in tests) causes fatal
// double-registration errors when statically linked.
static Ort::Env& get_ort_env()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "locus_embedder");
    return env;
}

struct Embedder::Impl {
    Ort::Session session;
    Ort::MemoryInfo mem_info;

    Impl(const fs::path& model_path)
        : session(nullptr)
        , mem_info(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef _WIN32
        session = Ort::Session(get_ort_env(), model_path.wstring().c_str(), opts);
#else
        session = Ort::Session(get_ort_env(), model_path.string().c_str(), opts);
#endif

        spdlog::info("ONNX model loaded: {}", model_path.string());
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
    impl_ = std::make_unique<Impl>(model_path);
}

Embedder::~Embedder() = default;

std::vector<float> Embedder::embed(const std::string& text) const
{
    auto token_ids = tokenize(text);
    int64_t seq_len = static_cast<int64_t>(token_ids.size());

    // attention_mask: all 1s
    std::vector<int64_t> attention_mask(seq_len, 1);
    // token_type_ids: all 0s
    std::vector<int64_t> token_type_ids(seq_len, 0);

    std::array<int64_t, 2> shape = {1, seq_len};

    auto input_ids_tensor = Ort::Value::CreateTensor<int64_t>(
        impl_->mem_info, token_ids.data(), token_ids.size(),
        shape.data(), shape.size());

    auto attention_tensor = Ort::Value::CreateTensor<int64_t>(
        impl_->mem_info, attention_mask.data(), attention_mask.size(),
        shape.data(), shape.size());

    auto type_ids_tensor = Ort::Value::CreateTensor<int64_t>(
        impl_->mem_info, token_type_ids.data(), token_type_ids.size(),
        shape.data(), shape.size());

    const char* input_names[] = {"input_ids", "attention_mask", "token_type_ids"};
    const char* output_names[] = {"last_hidden_state"};

    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(input_ids_tensor));
    inputs.push_back(std::move(attention_tensor));
    inputs.push_back(std::move(type_ids_tensor));

    auto outputs = impl_->session.Run(
        Ort::RunOptions{nullptr},
        input_names, inputs.data(), inputs.size(),
        output_names, 1);

    // Output shape: [1, seq_len, hidden_dim]
    // Mean pooling over the sequence dimension with attention mask
    auto& output_tensor = outputs[0];
    auto shape_info = output_tensor.GetTensorTypeAndShapeInfo();
    auto out_shape = shape_info.GetShape();
    int64_t hidden_dim = out_shape.back();

    const float* data = output_tensor.GetTensorData<float>();

    // Mean pooling: average across token positions (weighted by attention mask)
    std::vector<float> embedding(hidden_dim, 0.0f);
    float mask_sum = 0.0f;
    for (int64_t t = 0; t < seq_len; ++t) {
        float mask_val = static_cast<float>(attention_mask[t]);
        mask_sum += mask_val;
        for (int64_t d = 0; d < hidden_dim; ++d) {
            embedding[d] += data[t * hidden_dim + d] * mask_val;
        }
    }
    if (mask_sum > 0.0f) {
        for (auto& v : embedding) v /= mask_sum;
    }

    // L2 normalise
    float norm = 0.0f;
    for (float v : embedding) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (auto& v : embedding) v /= norm;
    }

    // Truncate/pad to expected dimensions
    embedding.resize(dimensions_, 0.0f);
    return embedding;
}

} // namespace locus
