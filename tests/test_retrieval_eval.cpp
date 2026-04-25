// Pure-math tests for the retrieval eval metrics. The full harness is
// manual-only (locus_retrieval_eval) — these unit tests guard the metric
// logic so a refactor doesn't quietly invert recall or break nDCG.

#include "../tests/retrieval_eval/metrics.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace locus::eval;

TEST_CASE("recall@K hits at exact rank K", "[s4.k][metrics]")
{
    std::unordered_set<std::string> relevant = {"a.cpp"};
    std::vector<std::string> retrieved = {"x.cpp", "y.cpp", "a.cpp", "z.cpp"};

    auto m = compute(retrieved, relevant, {1, 3, 5, 10});

    CHECK(m.recall_at_k[0] == 0.0);  // rank 3, not in top 1
    CHECK(m.recall_at_k[1] == 1.0);  // rank 3, in top 3
    CHECK(m.recall_at_k[2] == 1.0);
    CHECK(m.recall_at_k[3] == 1.0);
    CHECK(m.any_hit);
    CHECK_THAT(m.mrr, Catch::Matchers::WithinAbs(1.0 / 3.0, 1e-9));
}

TEST_CASE("multiple expected files; partial recall", "[s4.k][metrics]")
{
    std::unordered_set<std::string> relevant = {"a.cpp", "b.cpp", "c.cpp"};
    std::vector<std::string> retrieved = {"a.cpp", "x.cpp", "b.cpp", "y.cpp", "z.cpp"};

    auto m = compute(retrieved, relevant, {1, 3, 5});

    // R@1: 1/3 (a hit), R@3: 2/3 (a + b), R@5: 2/3
    CHECK_THAT(m.recall_at_k[0], Catch::Matchers::WithinAbs(1.0 / 3.0, 1e-9));
    CHECK_THAT(m.recall_at_k[1], Catch::Matchers::WithinAbs(2.0 / 3.0, 1e-9));
    CHECK_THAT(m.recall_at_k[2], Catch::Matchers::WithinAbs(2.0 / 3.0, 1e-9));
    // MRR uses first hit at rank 1.
    CHECK(m.mrr == 1.0);
}

TEST_CASE("no hits", "[s4.k][metrics]")
{
    std::unordered_set<std::string> relevant = {"a.cpp"};
    std::vector<std::string> retrieved = {"x.cpp", "y.cpp", "z.cpp"};

    auto m = compute(retrieved, relevant, {1, 3, 10});

    CHECK(m.recall_at_k[0] == 0.0);
    CHECK(m.recall_at_k[1] == 0.0);
    CHECK(m.recall_at_k[2] == 0.0);
    CHECK_FALSE(m.any_hit);
    CHECK(m.mrr == 0.0);
    CHECK(m.ndcg_at_10 == 0.0);
}

TEST_CASE("duplicate hit is not double-counted", "[s4.k][metrics]")
{
    std::unordered_set<std::string> relevant = {"a.cpp"};
    std::vector<std::string> retrieved = {"a.cpp", "a.cpp", "a.cpp"};

    auto m = compute(retrieved, relevant, {1, 3});

    CHECK(m.recall_at_k[0] == 1.0);
    CHECK(m.recall_at_k[1] == 1.0);  // not 3.0 — bounded by relevant.size()
    CHECK(m.mrr == 1.0);
}

TEST_CASE("nDCG@10 perfect ordering is 1.0", "[s4.k][metrics]")
{
    std::unordered_set<std::string> relevant = {"a.cpp", "b.cpp"};
    std::vector<std::string> retrieved = {"a.cpp", "b.cpp", "x.cpp", "y.cpp"};

    auto m = compute(retrieved, relevant, {1, 3, 10});
    CHECK_THAT(m.ndcg_at_10, Catch::Matchers::WithinAbs(1.0, 1e-9));
}

TEST_CASE("nDCG@10 reversed ordering is between 0 and 1", "[s4.k][metrics]")
{
    std::unordered_set<std::string> relevant = {"a.cpp", "b.cpp"};
    // Hits at ranks 9 and 10 — late but inside the cutoff
    std::vector<std::string> retrieved = {
        "x1.cpp", "x2.cpp", "x3.cpp", "x4.cpp",
        "x5.cpp", "x6.cpp", "x7.cpp", "x8.cpp",
        "a.cpp", "b.cpp"
    };

    auto m = compute(retrieved, relevant, {1, 3, 10});
    CHECK(m.ndcg_at_10 > 0.0);
    CHECK(m.ndcg_at_10 < 1.0);
}

TEST_CASE("aggregate averages metrics across queries", "[s4.k][metrics]")
{
    QueryMetrics q1, q2;
    q1.recall_at_k = {1.0, 1.0};
    q1.mrr = 1.0; q1.ndcg_at_10 = 1.0; q1.any_hit = true;
    q2.recall_at_k = {0.0, 0.5};
    q2.mrr = 0.5; q2.ndcg_at_10 = 0.5; q2.any_hit = true;

    auto agg = aggregate({q1, q2}, {1, 3});

    CHECK_THAT(agg.mean_recall_at_k[0], Catch::Matchers::WithinAbs(0.5,  1e-9));
    CHECK_THAT(agg.mean_recall_at_k[1], Catch::Matchers::WithinAbs(0.75, 1e-9));
    CHECK_THAT(agg.mean_mrr,            Catch::Matchers::WithinAbs(0.75, 1e-9));
    CHECK_THAT(agg.mean_ndcg_at_10,     Catch::Matchers::WithinAbs(0.75, 1e-9));
    CHECK(agg.queries == 2);
    CHECK(agg.queries_any_hit == 2);
}
