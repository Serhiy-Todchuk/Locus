#include "metrics.h"

#include <algorithm>
#include <cmath>

namespace locus::eval {

QueryMetrics compute(const std::vector<std::string>&        retrieved,
                     const std::unordered_set<std::string>& relevant,
                     const std::vector<int>&                ks)
{
    QueryMetrics m;
    m.recall_at_k.assign(ks.size(), 0.0);
    if (relevant.empty()) {
        return m;  // ill-defined; caller filters these
    }

    int max_k = 0;
    for (int k : ks) max_k = std::max(max_k, k);
    max_k = std::max(max_k, 10);  // nDCG@10 needs at least 10 ranks

    // Walk the retrieved list once, deduplicated, recording the first rank at
    // which each relevant file appears (1-based).
    std::unordered_set<std::string> seen;
    std::vector<int> hit_ranks;
    int rank = 0;
    for (const auto& path : retrieved) {
        if (seen.count(path)) continue;
        seen.insert(path);
        ++rank;
        if (rank > max_k) break;
        if (relevant.count(path)) {
            hit_ranks.push_back(rank);
        }
    }

    if (!hit_ranks.empty()) {
        m.any_hit = true;
        m.mrr = 1.0 / static_cast<double>(hit_ranks.front());
    }

    for (size_t i = 0; i < ks.size(); ++i) {
        int k = ks[i];
        int hits = 0;
        for (int r : hit_ranks) if (r <= k) ++hits;
        m.recall_at_k[i] = static_cast<double>(hits) /
                           static_cast<double>(relevant.size());
    }

    // nDCG@10 with binary relevance (gain = 1 for hit, 0 for miss).
    // DCG = sum_{i=1..10} rel_i / log2(i + 1)
    // IDCG = sum_{i=1..min(10, |relevant|)} 1 / log2(i + 1)
    double dcg = 0.0;
    for (int r : hit_ranks) {
        if (r <= 10) dcg += 1.0 / std::log2(static_cast<double>(r) + 1.0);
    }
    int ideal_hits = std::min<int>(10, static_cast<int>(relevant.size()));
    double idcg = 0.0;
    for (int i = 1; i <= ideal_hits; ++i) {
        idcg += 1.0 / std::log2(static_cast<double>(i) + 1.0);
    }
    m.ndcg_at_10 = (idcg > 0.0) ? (dcg / idcg) : 0.0;

    return m;
}

AggregateMetrics aggregate(const std::vector<QueryMetrics>& per_query,
                           const std::vector<int>&          ks)
{
    AggregateMetrics agg;
    agg.mean_recall_at_k.assign(ks.size(), 0.0);
    agg.queries = static_cast<int>(per_query.size());
    if (per_query.empty()) return agg;

    for (const auto& q : per_query) {
        for (size_t i = 0; i < ks.size() && i < q.recall_at_k.size(); ++i) {
            agg.mean_recall_at_k[i] += q.recall_at_k[i];
        }
        agg.mean_mrr        += q.mrr;
        agg.mean_ndcg_at_10 += q.ndcg_at_10;
        if (q.any_hit) ++agg.queries_any_hit;
    }
    double n = static_cast<double>(per_query.size());
    for (auto& v : agg.mean_recall_at_k) v /= n;
    agg.mean_mrr        /= n;
    agg.mean_ndcg_at_10 /= n;
    return agg;
}

} // namespace locus::eval
