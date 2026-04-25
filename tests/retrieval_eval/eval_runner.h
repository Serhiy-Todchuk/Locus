#pragma once

#include "metrics.h"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace locus {
class Workspace;
}

namespace locus::eval {

namespace fs = std::filesystem;

struct GoldQuery {
    std::string                     query;
    std::unordered_set<std::string> expected_files;  // forward-slash relative
};

struct PerMethodResult {
    // Aligned with `queries`: per_query[i] = metrics for queries[i] under this method.
    std::vector<QueryMetrics> per_query;
    AggregateMetrics          aggregate;
    long long                 wall_ms = 0;  // total wall time across all queries
    bool                      enabled = false;
    std::string               disabled_reason;  // populated when enabled=false
};

struct EvalRun {
    std::vector<GoldQuery> queries;
    std::vector<int>       ks;          // {1, 3, 5, 10}
    PerMethodResult        text;
    PerMethodResult        semantic;
    PerMethodResult        hybrid;
};

// Load the queries.json gold file. Throws std::runtime_error on parse failure
// or empty `queries` array. Path strings are normalised to forward slashes so
// they match what IndexQuery returns.
std::vector<GoldQuery> load_queries(const fs::path& queries_json);

// Run all gold queries through search_text / search_semantic / search_hybrid
// against `ws`. `top_k` is the number of candidates each method retrieves
// (caller picks a value at least as large as the largest k in `ks`). Methods
// that the workspace can't run (no embedder, no vectors db) are reported as
// `enabled=false` rather than skipped silently.
EvalRun run_all(Workspace& ws, const std::vector<GoldQuery>& queries,
                const std::vector<int>& ks, int top_k);

// Render the run as a markdown report. Header includes wall-clock per method
// and total query count. The body has one row per method (text/semantic/hybrid)
// followed by a per-query table that lists which method recovered each query
// at K=3 (helps spot queries whose ground truth is wrong vs. genuinely hard).
std::string format_markdown(const EvalRun& run, const std::string& workspace_root);

// Strip the EvalRun down to a JSON shape suitable for diffing across runs:
// per-method aggregate metrics only (no per-query detail). Used for the
// committed baseline.json.
std::string format_baseline_json(const EvalRun& run);

// Compare a current run against a previously saved baseline. Returns a list of
// human-readable regression lines (empty = no regression). `tolerance` is the
// fractional drop a metric is allowed without flagging (0.05 = 5%).
struct Regression {
    std::string method;
    std::string metric;
    double      baseline = 0.0;
    double      current  = 0.0;
};

std::vector<Regression>
detect_regressions(const std::string& baseline_json,
                   const EvalRun& current,
                   double tolerance);

} // namespace locus::eval
