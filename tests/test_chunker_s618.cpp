#include <catch2/catch_test_macros.hpp>

#include "index/chunker.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

using namespace locus;

// S6.18 Task A.2 -- chunk_code defensive walk against degenerate spans.
// The TestLocalVibe2 vectors.db revealed that for a 3-file workspace the
// chunker emitted 238 chunks with 220 overlapping. Root cause: the symbol
// extractor emits 112 symbols for big_module.cpp -- 111 nested under a
// top-level namespace -- and the old chunker's cursor-clamp inverted
// `sym_begin >= sym_end` ranges, producing chunks with start_line > end_line
// and empty content while the cursor moved backwards on every nested span.
//
// Together with Task A.3's leaf-symbol pre-filter (lives in indexer.cpp),
// the chunker no longer sees containers; this file only verifies the
// chunker's own defensive walk, given a pre-filtered span list.

namespace {

std::string make_lines(int n)
{
    std::ostringstream ss;
    for (int i = 1; i <= n; ++i) {
        ss << "line " << i << "\n";
    }
    return ss.str();
}

bool has_empty(const std::vector<Chunk>& chunks)
{
    return std::any_of(chunks.begin(), chunks.end(),
                       [](const Chunk& c) { return c.content.empty(); });
}

bool has_inverted_range(const std::vector<Chunk>& chunks)
{
    return std::any_of(chunks.begin(), chunks.end(),
                       [](const Chunk& c) { return c.start_line > c.end_line; });
}

bool monotonic_starts(const std::vector<Chunk>& chunks)
{
    int last = 0;
    for (const auto& c : chunks) {
        if (c.start_line < last) return false;
        last = c.start_line;
    }
    return true;
}

} // namespace

TEST_CASE("chunk_code rejects sym_begin >= sym_end degenerate spans", "[s6.18][chunker]")
{
    // Span 2 starts AFTER span 1 ends but inside the cursor: pre-cursor-clamp
    // produces sym_begin == sym_end which the old code chunked into [start=N+1,
    // end=N, content=""] then moved the cursor backward.
    auto content = make_lines(40);
    std::vector<SymbolSpan> spans = {
        {1, 30},   // leaf 1
        {15, 20},  // contained by leaf 1 -- after cursor clamp begin=cursor=30,
                   //                       sym_end=20 -> inverted
        {30, 40},  // legitimate trailing leaf
    };
    auto chunks = chunk_code(content, spans, /*max_lines=*/100, /*overlap=*/0);

    REQUIRE(!chunks.empty());
    REQUIRE_FALSE(has_empty(chunks));
    REQUIRE_FALSE(has_inverted_range(chunks));
    REQUIRE(monotonic_starts(chunks));
}

TEST_CASE("chunk_code nested namespace + classes produces no zero-length chunks",
          "[s6.18][chunker]")
{
    // A miniature of the TestLocalVibe2 big_module.cpp shape: namespace
    // wraps everything, two classes wrap their methods. Without leaf-symbol
    // pre-filtering the chunker would see all 7 spans (1 ns + 2 cls + 4
    // methods); pre-filtering keeps only the 4 leaves. Verify the chunker
    // emits clean chunks for the leaf-only input.
    auto content = make_lines(60);
    std::vector<SymbolSpan> leaf_only = {
        {3,  12},   // Foo::foo_a
        {14, 22},   // Foo::foo_b
        {25, 35},   // Bar::bar_a
        {38, 50},   // Bar::bar_b
    };
    auto chunks = chunk_code(content, leaf_only, /*max_lines=*/100, /*overlap=*/0);

    REQUIRE(!chunks.empty());
    REQUIRE_FALSE(has_empty(chunks));
    REQUIRE_FALSE(has_inverted_range(chunks));
    REQUIRE(monotonic_starts(chunks));
}

TEST_CASE("chunk_code does not move cursor backwards on overlapping spans",
          "[s6.18][chunker]")
{
    // After the cursor lands at line 30 due to a wide span, a subsequent span
    // ending at line 18 must not pull the cursor back to 18 (the old bug) or
    // produce a chunk that re-emits lines 18..30 a second time.
    auto content = make_lines(50);
    std::vector<SymbolSpan> spans = {
        {1, 30},   // wide leaf; cursor goes to 30
        {10, 18},  // legacy: would have set cursor=18 and re-emitted
        {35, 50},  // trailing
    };
    auto chunks = chunk_code(content, spans, /*max_lines=*/100, /*overlap=*/0);

    REQUIRE(monotonic_starts(chunks));
    REQUIRE_FALSE(has_empty(chunks));
    REQUIRE_FALSE(has_inverted_range(chunks));
}

// S6.18 Task A.4 -- belt-and-suspenders against empty chunks. The chunker's
// internal helpers (make_chunk / sliding_split) refuse `begin >= end` and
// return nothing.

TEST_CASE("chunk_sliding_window handles empty content cleanly", "[s6.18][chunker]")
{
    auto chunks = chunk_sliding_window("", 10, 2);
    REQUIRE(chunks.empty());
}

TEST_CASE("chunk_code high-overlap config does not produce empty chunks",
          "[s6.18][chunker]")
{
    // overlap >= max_lines tickles the sliding-split step calculation
    // (`max_lines - overlap` clamps to 1 to avoid zero-step infinite loop)
    // -- verify it doesn't synthesize empties either.
    auto content = make_lines(20);
    std::vector<SymbolSpan> spans = { {1, 20} };
    auto chunks = chunk_code(content, spans, /*max_lines=*/5, /*overlap=*/10);

    REQUIRE(!chunks.empty());
    REQUIRE_FALSE(has_empty(chunks));
    REQUIRE_FALSE(has_inverted_range(chunks));
}
