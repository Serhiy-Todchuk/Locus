#include "eval_runner.h"

#include "embedder.h"
#include "embedding_worker.h"
#include "index/index_query.h"
#include "workspace.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace locus::eval {

using json = nlohmann::json;

namespace {

std::string normalise_slashes(std::string p)
{
    for (auto& c : p) if (c == '\\') c = '/';
    return p;
}

// Normalised single-token path for FTS5: replaces non-alphanumeric chars with
// spaces so queries like "src/tools/file_tools.cpp" don't tokenise as the
// special FTS column-spec syntax. Caller wraps the result in quotes.
std::string sanitise_for_fts(const std::string& q)
{
    // Drop FTS5-meta characters; bm25() tokenises on whitespace anyway.
    static const std::string strip = "\"'():*-^!";
    std::string out;
    out.reserve(q.size());
    for (char c : q) {
        if (strip.find(c) != std::string::npos) out += ' ';
        else                                    out += c;
    }
    return out;
}

std::vector<std::string>
extract_paths(const std::vector<SearchResult>& results)
{
    std::vector<std::string> out;
    out.reserve(results.size());
    for (const auto& r : results) out.push_back(normalise_slashes(r.path));
    return out;
}

} // namespace

// -- Loading ------------------------------------------------------------------

std::vector<GoldQuery> load_queries(const fs::path& queries_json)
{
    std::ifstream f(queries_json);
    if (!f.is_open())
        throw std::runtime_error("Cannot open queries file: " + queries_json.string());

    json j;
    try {
        j = json::parse(f);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("Invalid queries.json: ") + e.what());
    }

    if (!j.contains("queries") || !j["queries"].is_array())
        throw std::runtime_error("queries.json missing 'queries' array");

    std::vector<GoldQuery> out;
    int idx = 0;
    for (const auto& entry : j["queries"]) {
        ++idx;
        if (!entry.contains("query") || !entry["query"].is_string()) {
            spdlog::warn("queries.json[{}]: missing 'query' string — skipped", idx);
            continue;
        }
        if (!entry.contains("expected_files") || !entry["expected_files"].is_array()) {
            spdlog::warn("queries.json[{}]: missing 'expected_files' array — skipped", idx);
            continue;
        }
        GoldQuery g;
        g.query = entry["query"].get<std::string>();
        for (const auto& p : entry["expected_files"]) {
            if (p.is_string())
                g.expected_files.insert(normalise_slashes(p.get<std::string>()));
        }
        if (g.expected_files.empty()) {
            spdlog::warn("queries.json[{}]: 'expected_files' empty for '{}' — skipped",
                         idx, g.query);
            continue;
        }
        out.push_back(std::move(g));
    }

    if (out.empty())
        throw std::runtime_error("queries.json has no usable queries");
    return out;
}

// -- Run ----------------------------------------------------------------------

EvalRun run_all(Workspace& ws, const std::vector<GoldQuery>& queries,
                const std::vector<int>& ks, int top_k)
{
    EvalRun run;
    run.queries = queries;
    run.ks      = ks;

    auto* idx = ws.index();
    if (!idx)
        throw std::runtime_error("Workspace has no IndexQuery — cannot run eval");

    // Decide which methods are runnable.
    run.text.enabled     = true;
    run.semantic.enabled = (ws.embedding_worker() != nullptr) &&
                           (ws.vectors_database() != nullptr);
    run.hybrid.enabled   = run.semantic.enabled;
    if (!run.semantic.enabled) {
        run.semantic.disabled_reason = "no embedder / vectors.db";
        run.hybrid.disabled_reason   = "no embedder / vectors.db";
    }

    SearchOptions opts;
    opts.max_results = top_k;

    auto* emb_worker = ws.embedding_worker();

    auto run_method = [&](PerMethodResult& m,
                          auto&& fetch /* (query) -> vector<string> paths */)
    {
        if (!m.enabled) return;
        auto t0 = std::chrono::steady_clock::now();
        m.per_query.reserve(queries.size());
        for (const auto& q : queries) {
            std::vector<std::string> hits = fetch(q.query);
            m.per_query.push_back(compute(hits, q.expected_files, ks));
        }
        auto t1 = std::chrono::steady_clock::now();
        m.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        m.aggregate = aggregate(m.per_query, ks);
    };

    spdlog::info("Eval: running {} queries × text", queries.size());
    run_method(run.text, [&](const std::string& q) {
        // FTS5 takes the raw user query as a MATCH expression. We sanitise the
        // operators it would otherwise consume (colon, quote, etc.) and let the
        // tokenizer split on whitespace.
        return extract_paths(idx->search_text(sanitise_for_fts(q), opts));
    });

    if (run.semantic.enabled) {
        spdlog::info("Eval: running {} queries × semantic", queries.size());
        run_method(run.semantic, [&](const std::string& q) {
            auto vec = emb_worker->embed_query(q);
            return extract_paths(idx->search_semantic(vec, opts));
        });
    }
    if (run.hybrid.enabled) {
        spdlog::info("Eval: running {} queries × hybrid", queries.size());
        run_method(run.hybrid, [&](const std::string& q) {
            auto vec = emb_worker->embed_query(q);
            return extract_paths(
                idx->search_hybrid(sanitise_for_fts(q), vec, opts));
        });
    }

    return run;
}

// -- Markdown report ----------------------------------------------------------

namespace {

std::string fmt_pct(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << (v * 100.0) << "%";
    return os.str();
}

std::string fmt_dec(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << v;
    return os.str();
}

void append_method_row(std::string& out, const char* name,
                       const PerMethodResult& m, const std::vector<int>& ks)
{
    out += "| ";
    out += name;
    out += " |";
    if (!m.enabled) {
        for (size_t i = 0; i < ks.size(); ++i) out += " — |";
        out += " — | — | — | (";
        out += m.disabled_reason;
        out += ") |\n";
        return;
    }
    for (size_t i = 0; i < ks.size(); ++i) {
        out += " ";
        out += fmt_pct(m.aggregate.mean_recall_at_k[i]);
        out += " |";
    }
    out += " ";
    out += fmt_dec(m.aggregate.mean_mrr);
    out += " | ";
    out += fmt_dec(m.aggregate.mean_ndcg_at_10);
    out += " | ";
    out += std::to_string(m.aggregate.queries_any_hit);
    out += "/";
    out += std::to_string(m.aggregate.queries);
    out += " | ";
    out += std::to_string(m.wall_ms);
    out += " ms |\n";
}

} // namespace

std::string format_markdown(const EvalRun& run, const std::string& workspace_root)
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream stamp;
    stamp << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

    std::string out;
    out += "# Locus Retrieval Eval\n\n";
    out += "Generated: " + stamp.str() + "\n";
    out += "Workspace: `" + workspace_root + "`\n";
    out += "Queries:   " + std::to_string(run.queries.size()) + "\n\n";

    out += "## Aggregate metrics\n\n";
    out += "| Method |";
    for (int k : run.ks) {
        out += " R@";
        out += std::to_string(k);
        out += " |";
    }
    out += " MRR | nDCG@10 | hit | wall |\n";
    out += "|---|";
    for (size_t i = 0; i < run.ks.size(); ++i) out += "---|";
    out += "---|---|---|---|\n";

    append_method_row(out, "search_text",     run.text,     run.ks);
    append_method_row(out, "search_semantic", run.semantic, run.ks);
    append_method_row(out, "search_hybrid",   run.hybrid,   run.ks);

    out += "\n_R@K = recall@K (mean across queries). hit = queries with ≥1 "
           "expected file in top-K_max. wall = total wall time spent inside "
           "this method's calls._\n";

    // Per-query table — useful to spot queries whose ground truth is wrong vs.
    // genuinely hard. Mark with ✓ when the method recovered ≥1 expected file
    // within rank 3 (a tight bar; many "hard" queries hit at rank 5–10).
    out += "\n## Per-query — recall@3 hits\n\n";
    out += "| # | Query | text | semantic | hybrid |\n";
    out += "|---|---|---|---|---|\n";

    auto cell = [&](const PerMethodResult& m, size_t qi) -> std::string {
        if (!m.enabled || qi >= m.per_query.size()) return "—";
        // recall_at_k[1] corresponds to ks[1]; assume ks[1] == 3 (caller controls).
        size_t i3 = 0;
        for (size_t i = 0; i < run.ks.size(); ++i) {
            if (run.ks[i] == 3) { i3 = i; break; }
        }
        double r3 = m.per_query[qi].recall_at_k[i3];
        return r3 > 0.0 ? "✓" : "·";
    };

    for (size_t i = 0; i < run.queries.size(); ++i) {
        out += "| ";
        out += std::to_string(i + 1);
        out += " | ";
        // Markdown-escape pipe in queries — none expected, but be safe.
        std::string q = run.queries[i].query;
        std::replace(q.begin(), q.end(), '|', '/');
        out += q;
        out += " | ";
        out += cell(run.text, i);
        out += " | ";
        out += cell(run.semantic, i);
        out += " | ";
        out += cell(run.hybrid, i);
        out += " |\n";
    }
    out += "\n";
    return out;
}

// -- Baseline JSON ------------------------------------------------------------

namespace {

json method_to_json(const PerMethodResult& m, const std::vector<int>& ks)
{
    json j;
    j["enabled"] = m.enabled;
    if (!m.enabled) {
        j["disabled_reason"] = m.disabled_reason;
        return j;
    }
    json r = json::object();
    for (size_t i = 0; i < ks.size(); ++i) {
        r[std::to_string(ks[i])] = m.aggregate.mean_recall_at_k[i];
    }
    j["recall_at_k"] = std::move(r);
    j["mrr"]         = m.aggregate.mean_mrr;
    j["ndcg_at_10"]  = m.aggregate.mean_ndcg_at_10;
    j["queries_any_hit"] = m.aggregate.queries_any_hit;
    j["queries"]         = m.aggregate.queries;
    return j;
}

} // namespace

std::string format_baseline_json(const EvalRun& run)
{
    json doc;
    doc["version"]      = 1;
    doc["query_count"]  = static_cast<int>(run.queries.size());
    doc["text"]         = method_to_json(run.text,     run.ks);
    doc["semantic"]     = method_to_json(run.semantic, run.ks);
    doc["hybrid"]       = method_to_json(run.hybrid,   run.ks);
    return doc.dump(2);
}

// -- Regression detection -----------------------------------------------------

namespace {

void compare_metric(std::vector<Regression>& out, const std::string& method,
                    const std::string& metric, double baseline, double current,
                    double tolerance)
{
    // A drop > tolerance triggers. We do NOT flag improvements (that would be
    // counter-productive). Floor of 0.001 avoids spurious triggers when the
    // baseline metric was already near zero.
    if (baseline < 0.001) return;
    if ((baseline - current) / baseline > tolerance) {
        out.push_back({method, metric, baseline, current});
    }
}

void compare_method(std::vector<Regression>& out, const std::string& method,
                    const json& base, const PerMethodResult& cur,
                    const std::vector<int>& ks, double tolerance)
{
    if (!cur.enabled) return;        // current run can't compare; not a regression
    if (!base.is_object()) return;
    if (!base.value("enabled", false)) return;

    if (base.contains("recall_at_k")) {
        for (size_t i = 0; i < ks.size(); ++i) {
            std::string key = std::to_string(ks[i]);
            if (!base["recall_at_k"].contains(key)) continue;
            double b = base["recall_at_k"][key].get<double>();
            double c = cur.aggregate.mean_recall_at_k[i];
            compare_metric(out, method, "recall@" + key, b, c, tolerance);
        }
    }
    if (base.contains("mrr")) {
        compare_metric(out, method, "mrr",
                       base["mrr"].get<double>(),
                       cur.aggregate.mean_mrr, tolerance);
    }
    if (base.contains("ndcg_at_10")) {
        compare_metric(out, method, "ndcg@10",
                       base["ndcg_at_10"].get<double>(),
                       cur.aggregate.mean_ndcg_at_10, tolerance);
    }
}

} // namespace

std::vector<Regression>
detect_regressions(const std::string& baseline_json,
                   const EvalRun& current,
                   double tolerance)
{
    std::vector<Regression> out;
    if (baseline_json.empty()) return out;

    json base;
    try {
        base = json::parse(baseline_json);
    } catch (const json::exception& e) {
        spdlog::warn("Eval: baseline.json invalid ({}); skipping regression check",
                     e.what());
        return out;
    }

    compare_method(out, "search_text",     base.value("text",     json::object()),
                   current.text,     current.ks, tolerance);
    compare_method(out, "search_semantic", base.value("semantic", json::object()),
                   current.semantic, current.ks, tolerance);
    compare_method(out, "search_hybrid",   base.value("hybrid",   json::object()),
                   current.hybrid,   current.ks, tolerance);
    return out;
}

} // namespace locus::eval
