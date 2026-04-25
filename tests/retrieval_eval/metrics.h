#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace locus::eval {

// File-level retrieval metrics. The retrieved list is treated as an ordered
// sequence of candidate paths (rank 1 = first); duplicates after the first
// occurrence are ignored so a method that ranks the same file twice doesn't
// inflate recall.
struct QueryMetrics {
    // Per-K recall: |relevant ∩ retrieved[0..K)| / |relevant|.
    // Indices map 1:1 with the K values passed into compute().
    std::vector<double> recall_at_k;
    // Reciprocal rank of the first relevant hit, 0.0 if none in the truncated
    // list. Truncation cap is the largest K passed in.
    double mrr = 0.0;
    // Normalised DCG@10. Binary relevance, uniform gain. 0.0 if no relevant
    // hits in the top-10.
    double ndcg_at_10 = 0.0;
    // True iff at least one expected file was retrieved at any rank within the
    // truncation cap. Useful to spot queries that bottom out (model has zero
    // signal for them).
    bool   any_hit = false;
};

QueryMetrics compute(const std::vector<std::string>&        retrieved,
                     const std::unordered_set<std::string>& relevant,
                     const std::vector<int>&                ks);

// Aggregated metrics across many queries — element-wise mean. Empty-input
// produces an all-zero result.
struct AggregateMetrics {
    std::vector<double> mean_recall_at_k;
    double mean_mrr        = 0.0;
    double mean_ndcg_at_10 = 0.0;
    int    queries         = 0;
    int    queries_any_hit = 0;
};

AggregateMetrics aggregate(const std::vector<QueryMetrics>& per_query,
                           const std::vector<int>&          ks);

} // namespace locus::eval
