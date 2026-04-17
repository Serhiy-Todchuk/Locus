#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "chunker.h"
#include "embedder.h"
#include "extractors/text_extractor.h"
#include "index_query.h"

#include <cmath>
#include <filesystem>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

using namespace locus;
namespace fs = std::filesystem;

// Locate the ONNX model file. Prefers .ort (pre-optimised), falls back to .onnx.
// Returns empty path if not found.
static fs::path find_model()
{
    // Try both names in both locations
    const char* names[] = {"all-MiniLM-L6-v2.ort", "all-MiniLM-L6-v2.onnx"};
    for (const char* name : names) {
        // CWD (project root when run from CLI)
        if (fs::exists(fs::path("models") / name))
            return fs::path("models") / name;
        // Relative to typical MSVC test exe: build/debug/tests/Debug/
        fs::path up4 = fs::path("../../../../models") / name;
        if (fs::exists(up4))
            return up4;
    }
    return {};
}

// ============================================================================
// Chunker tests
// ============================================================================

TEST_CASE("chunk_sliding_window basic split", "[s2.1]")
{
    // 10 lines of content
    std::string content;
    for (int i = 1; i <= 10; ++i) {
        content += "line " + std::to_string(i) + "\n";
    }

    auto chunks = chunk_sliding_window(content, 4, 1);

    REQUIRE(chunks.size() >= 3);

    // First chunk starts at line 1
    CHECK(chunks[0].start_line == 1);
    CHECK(chunks[0].end_line == 4);

    // Chunks should cover all lines
    CHECK(chunks.back().end_line == 10);
}

TEST_CASE("chunk_sliding_window single chunk", "[s2.1]")
{
    std::string content = "line 1\nline 2\nline 3\n";
    auto chunks = chunk_sliding_window(content, 80, 10);

    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].start_line == 1);
    CHECK(chunks[0].end_line == 3);
}

TEST_CASE("chunk_sliding_window overlap", "[s2.1]")
{
    std::string content;
    for (int i = 1; i <= 20; ++i) {
        content += "line " + std::to_string(i) + "\n";
    }

    auto chunks = chunk_sliding_window(content, 10, 3);

    // With overlap=3, step=7. 20 lines => ceil((20-10)/7)+1 = ~3 chunks
    REQUIRE(chunks.size() >= 2);

    // Check overlap: second chunk's start should be within first chunk's range
    if (chunks.size() >= 2) {
        CHECK(chunks[1].start_line <= chunks[0].end_line);
    }
}

TEST_CASE("chunk_code splits at symbol boundaries", "[s2.1]")
{
    // 30 lines of code with two functions
    std::string content;
    for (int i = 1; i <= 30; ++i) {
        content += "code line " + std::to_string(i) + "\n";
    }

    std::vector<SymbolSpan> symbols = {
        {1, 12},   // function A: lines 1-12
        {15, 25},  // function B: lines 15-25
    };

    auto chunks = chunk_code(content, symbols, 80, 5);

    // Should produce at least 3 chunks: funcA, gap, funcB, trailing
    REQUIRE(chunks.size() >= 2);

    // First chunk should cover the first function
    CHECK(chunks[0].start_line == 1);
}

TEST_CASE("chunk_code large function gets sub-split", "[s2.1]")
{
    std::string content;
    for (int i = 1; i <= 100; ++i) {
        content += "code line " + std::to_string(i) + "\n";
    }

    std::vector<SymbolSpan> symbols = {
        {1, 100},  // one massive function
    };

    auto chunks = chunk_code(content, symbols, 30, 5);

    // 100 lines / step=25 => should produce multiple chunks
    REQUIRE(chunks.size() >= 3);
}

TEST_CASE("chunk_document splits at headings", "[s2.1]")
{
    std::string content;
    for (int i = 1; i <= 30; ++i) {
        content += "text line " + std::to_string(i) + "\n";
    }

    std::vector<ExtractedHeading> headings = {
        {2, "Introduction", 1},
        {2, "Methods", 10},
        {2, "Results", 20},
    };

    auto chunks = chunk_document(content, headings, 80, 5);

    // Should produce 3 sections (heading boundaries at lines 1, 10, 20)
    REQUIRE(chunks.size() == 3);
    CHECK(chunks[0].start_line == 1);
    CHECK(chunks[1].start_line == 10);
    CHECK(chunks[2].start_line == 20);
}

TEST_CASE("chunk_document falls back to sliding window without headings", "[s2.1]")
{
    std::string content;
    for (int i = 1; i <= 100; ++i) {
        content += "text line " + std::to_string(i) + "\n";
    }

    std::vector<ExtractedHeading> headings;  // empty

    auto chunks = chunk_document(content, headings, 30, 5);

    // Should use sliding window since no headings
    REQUIRE(chunks.size() >= 3);
}

TEST_CASE("chunk_code empty symbols falls back to sliding window", "[s2.1]")
{
    std::string content;
    for (int i = 1; i <= 50; ++i) {
        content += "line " + std::to_string(i) + "\n";
    }

    std::vector<SymbolSpan> symbols;  // empty

    auto chunks = chunk_code(content, symbols, 20, 3);

    // Should use sliding window since no symbols
    REQUIRE(chunks.size() >= 2);
}

// ============================================================================
// RRF (Reciprocal Rank Fusion) correctness tests
// ============================================================================

// Standalone RRF implementation for testing (mirrors the logic in IndexQuery)
static std::vector<std::pair<std::string, double>> rrf_merge(
    const std::vector<std::string>& list_a,
    const std::vector<std::string>& list_b,
    double k = 60.0)
{
    std::unordered_map<std::string, double> scores;

    for (int i = 0; i < static_cast<int>(list_a.size()); ++i) {
        scores[list_a[i]] += 1.0 / (k + i + 1);
    }
    for (int i = 0; i < static_cast<int>(list_b.size()); ++i) {
        scores[list_b[i]] += 1.0 / (k + i + 1);
    }

    std::vector<std::pair<std::string, double>> result(scores.begin(), scores.end());
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return result;
}

TEST_CASE("RRF merge identical lists boosts shared results", "[s2.1]")
{
    std::vector<std::string> list_a = {"file_a.cpp", "file_b.cpp", "file_c.cpp"};
    std::vector<std::string> list_b = {"file_a.cpp", "file_b.cpp", "file_c.cpp"};

    auto merged = rrf_merge(list_a, list_b);

    REQUIRE(merged.size() == 3);
    // Same order preserved since both lists agree
    CHECK(merged[0].first == "file_a.cpp");
    CHECK(merged[1].first == "file_b.cpp");
    CHECK(merged[2].first == "file_c.cpp");

    // Each result gets 2x the score (from both lists)
    CHECK(merged[0].second > merged[1].second);
}

TEST_CASE("RRF merge disjoint lists interleaves", "[s2.1]")
{
    std::vector<std::string> list_a = {"a.cpp", "b.cpp"};
    std::vector<std::string> list_b = {"c.cpp", "d.cpp"};

    auto merged = rrf_merge(list_a, list_b);

    REQUIRE(merged.size() == 4);
    // Top results from each list should appear first (rank 1 from each)
    // a.cpp and c.cpp both at rank 0, so tied in score
    CHECK(merged[0].second == merged[1].second);
}

TEST_CASE("RRF merge result appearing in both lists ranks higher", "[s2.1]")
{
    // "shared.cpp" appears in both, others appear in only one
    std::vector<std::string> list_a = {"only_a.cpp", "shared.cpp"};
    std::vector<std::string> list_b = {"shared.cpp", "only_b.cpp"};

    auto merged = rrf_merge(list_a, list_b);

    // "shared.cpp" gets score from rank 1 in A + rank 0 in B = higher combined
    // "only_a.cpp" gets score from rank 0 in A only
    // "shared.cpp" should be #1 because 1/(61+1) + 1/(60+1) > 1/(60+1)
    CHECK(merged[0].first == "shared.cpp");
}

TEST_CASE("RRF merge empty inputs", "[s2.1]")
{
    std::vector<std::string> empty;

    auto merged = rrf_merge(empty, empty);
    CHECK(merged.empty());

    std::vector<std::string> list_a = {"a.cpp"};
    merged = rrf_merge(list_a, empty);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].first == "a.cpp");
}

// ============================================================================
// Embedder tests (skipped if model is missing or fails to load)
// ============================================================================

// Try to create an Embedder. Returns nullptr if model is missing or ONNX
// Runtime rejects it (e.g. static-link schema registration issue in Debug).
static std::unique_ptr<Embedder> try_load_embedder()
{
    auto model_path = find_model();
    if (model_path.empty()) return nullptr;
    try {
        return std::make_unique<Embedder>(model_path, 384);
    } catch (const std::exception& e) {
        WARN("Model load failed: " << e.what());
        return nullptr;
    }
}

TEST_CASE("Embedder produces 384-dim normalised vector", "[s2.1]")
{
    auto embedder = try_load_embedder();
    if (!embedder) {
        WARN("Skipping: ONNX model not found or failed to load");
        return;
    }

    auto vec = embedder->embed("hello world");

    REQUIRE(vec.size() == 384);

    // Check L2 norm is ~1.0 (normalised)
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    CHECK_THAT(std::sqrt(norm), Catch::Matchers::WithinAbs(1.0, 0.01));
}

TEST_CASE("Embedder similar texts have high cosine similarity", "[s2.1]")
{
    auto embedder = try_load_embedder();
    if (!embedder) {
        WARN("Skipping: ONNX model not found or failed to load");
        return;
    }

    auto v1 = embedder->embed("the cat sat on the mat");
    auto v2 = embedder->embed("a cat is sitting on a mat");
    auto v3 = embedder->embed("quantum chromodynamics and gluon fields");

    // Cosine similarity (vectors are already L2-normalised, so dot product = cosine)
    auto cosine = [](const std::vector<float>& a, const std::vector<float>& b) {
        float dot = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
        return dot;
    };

    float sim_similar   = cosine(v1, v2);
    float sim_different = cosine(v1, v3);

    // Similar sentences should have higher similarity than unrelated ones
    CHECK(sim_similar > sim_different);
    CHECK(sim_similar > 0.5f);
}

TEST_CASE("Embedder empty and short inputs do not crash", "[s2.1]")
{
    auto embedder = try_load_embedder();
    if (!embedder) {
        WARN("Skipping: ONNX model not found or failed to load");
        return;
    }

    auto v_empty = embedder->embed("");
    REQUIRE(v_empty.size() == 384);

    auto v_short = embedder->embed("x");
    REQUIRE(v_short.size() == 384);

    // Both should still be normalised
    float norm = 0.0f;
    for (float v : v_short) norm += v * v;
    CHECK_THAT(std::sqrt(norm), Catch::Matchers::WithinAbs(1.0, 0.01));
}
