#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "agent/metrics.h"

#include <thread>

using namespace locus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("MetricsAggregator: empty state has sensible defaults", "[s4.s][metrics]")
{
    MetricsAggregator m;
    auto agg = m.aggregates();

    REQUIRE(agg.turn_count == 0);
    REQUIRE(agg.tokens_in_total == 0);
    REQUIRE(agg.tokens_out_total == 0);
    REQUIRE(agg.tokens_per_second == 0.0);
    REQUIRE(agg.avg_turn_ms == 0);
    REQUIRE(agg.retrieval_queries == 0);
    REQUIRE(agg.tool_calls_by_name.empty());
}

TEST_CASE("MetricsAggregator: turn timing aggregates", "[s4.s][metrics]")
{
    MetricsAggregator m;

    m.begin_turn(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    m.end_turn(/*had_error=*/false);

    m.begin_turn(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    m.end_turn(false);

    auto agg = m.aggregates();
    REQUIRE(agg.turn_count == 2);
    REQUIRE(agg.avg_turn_ms >= 5);
    REQUIRE(agg.max_turn_ms >= 5);
    REQUIRE(agg.turn_durations_ms.size() == 2);
}

TEST_CASE("MetricsAggregator: LLM step records server usage and stream throughput",
          "[s4.s][metrics]")
{
    MetricsAggregator m;
    m.begin_turn(1);

    CompletionUsage u;
    u.prompt_tokens = 100;
    u.completion_tokens = 50;
    u.total_tokens = 150;
    u.reasoning_tokens = 10;
    m.record_llm_step(u, /*stream_ms=*/1000, /*prev_total=*/0);

    m.end_turn(false);

    auto agg = m.aggregates();
    REQUIRE(agg.tokens_out_total == 50);
    REQUIRE(agg.reasoning_total == 10);
    // 50 completion tokens in 1s -> 50 tok/s
    REQUIRE(agg.tokens_per_second == Catch::Approx(50.0));
}

TEST_CASE("MetricsAggregator: tool histogram counts by name", "[s4.s][metrics]")
{
    MetricsAggregator m;
    m.begin_turn(1);
    m.record_tool("read_file",  true,  5,  100);
    m.record_tool("read_file",  true,  6,  200);
    m.record_tool("write_file", false, 3,  10);
    m.end_turn(false);

    auto agg = m.aggregates();
    REQUIRE(agg.tool_calls_by_name.size() == 2);
    // sorted descending — read_file (×2) first
    REQUIRE(agg.tool_calls_by_name[0].first == "read_file");
    REQUIRE(agg.tool_calls_by_name[0].second == 2);
    REQUIRE(agg.tool_calls_by_name[1].first == "write_file");
    REQUIRE(agg.tool_calls_by_name[1].second == 1);

    auto tools = m.tools_snapshot();
    REQUIRE(tools["write_file"].failures == 1);
    REQUIRE(tools["read_file"].failures == 0);
}

TEST_CASE("MetricsAggregator: retrieval hit rate from search tools", "[s4.s][metrics]")
{
    MetricsAggregator m;
    m.begin_turn(1);
    // 2 queries with hits, 1 empty
    m.record_tool("search", true, 10, 100, /*count=*/3,  /*cap=*/20);
    m.record_tool("search", true, 10, 100, /*count=*/0,  /*cap=*/20);
    m.record_tool("search", true, 10, 100, /*count=*/12, /*cap=*/20);
    m.end_turn(false);

    auto agg = m.aggregates();
    REQUIRE(agg.retrieval_queries == 3);
    REQUIRE(agg.retrieval_hit_rate == Catch::Approx(2.0/3.0));
}

TEST_CASE("MetricsAggregator: reset clears all state", "[s4.s][metrics]")
{
    MetricsAggregator m;
    m.begin_turn(1);
    m.record_tool("read_file", true, 5, 100);
    m.end_turn(false);

    m.reset();

    auto agg = m.aggregates();
    REQUIRE(agg.turn_count == 0);
    REQUIRE(agg.tool_calls_by_name.empty());
}

TEST_CASE("MetricsAggregator: JSON snapshot round-trip", "[s4.s][metrics]")
{
    MetricsAggregator m;
    m.begin_turn(1);
    CompletionUsage u; u.completion_tokens = 30; u.total_tokens = 30;
    m.record_llm_step(u, 100, 0);
    m.record_tool("search", true, 5, 50, 4, 20);
    m.end_turn(false);

    auto j = m.to_json();
    REQUIRE(j["turns"].is_array());
    REQUIRE(j["turns"].size() == 1);
    REQUIRE(j["tools"].contains("search"));
    REQUIRE(j["retrieval"]["queries"].get<int>() == 1);
    REQUIRE(j["totals"]["turn_count"].get<int>() == 1);
}

TEST_CASE("MetricsAggregator: CSV snapshot has section headers", "[s4.s][metrics]")
{
    MetricsAggregator m;
    m.begin_turn(1);
    m.record_tool("read_file", true, 1, 1);
    m.end_turn(false);

    auto csv = m.to_csv();
    REQUIRE_THAT(csv, ContainsSubstring("# turns"));
    REQUIRE_THAT(csv, ContainsSubstring("# tools"));
    REQUIRE_THAT(csv, ContainsSubstring("# retrieval"));
    REQUIRE_THAT(csv, ContainsSubstring("read_file"));
}

TEST_CASE("parse_search_result_count: leading integer extraction", "[s4.s][metrics]")
{
    REQUIRE(parse_search_result_count("search_text",
                                      "5 results for \"foo\"\n  bar.cpp:1\n").value() == 5);
    REQUIRE(parse_search_result_count("search_regex",
                                      "12 matches for /bar/ in 4 files (searched 100)\n").value() == 12);
    REQUIRE(parse_search_result_count("search_symbols",
                                      "3 symbols matching \"baz\"\n").value() == 3);
    REQUIRE(parse_search_result_count("search_semantic",
                                      "8 semantic matches:\n").value() == 8);
    REQUIRE(parse_search_result_count("search_hybrid",
                                      "10 hybrid matches (BM25 + semantic):\n").value() == 10);
}

TEST_CASE("parse_search_result_count: zero-result strings", "[s4.s][metrics]")
{
    REQUIRE(parse_search_result_count("search", "No matches found.").value() == 0);
    REQUIRE(parse_search_result_count("search_semantic",
                                      "No semantic matches found.").value() == 0);
}

TEST_CASE("parse_search_result_count: skips indexing-progress note", "[s4.s][metrics]")
{
    std::string content =
        "Note: indexing in progress (5/10 chunks embedded, 50%). "
        "Semantic results below are partial - retry once indexing "
        "completes for full coverage.\n\n"
        "7 semantic matches:\n[1] foo.cpp:1\n";
    auto count = parse_search_result_count("search_semantic", content);
    REQUIRE(count.has_value());
    REQUIRE(*count == 7);
}

TEST_CASE("parse_search_result_count: non-search tools return nullopt",
          "[s4.s][metrics]")
{
    REQUIRE_FALSE(parse_search_result_count("read_file", "5 lines\n").has_value());
    REQUIRE_FALSE(parse_search_result_count("write_file", "ok").has_value());
}
