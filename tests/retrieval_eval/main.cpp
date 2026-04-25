// locus_retrieval_eval — manual-only eval harness for hybrid retrieval.
//
//   locus_retrieval_eval [--workspace <dir>] [--queries <path>] [--out <path>]
//                        [--baseline <path>] [--top-k <N>] [--tolerance <f>]
//                        [--curate-from <sessions_dir>] [--verbose] [--no-wait]
//
// Returns:
//   0  — success, no regression
//   1  — regression detected (drop > tolerance vs. baseline)
//   2  — harness error (bad args, missing index, etc.)
//
// Defaults assume cwd is the repo root (the workspace under test).

#include "curate.h"
#include "eval_runner.h"

#include "embedding_worker.h"
#include "workspace.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace locus;

namespace {

struct Args {
    fs::path workspace      = fs::current_path();
    fs::path queries_path   = "tests/retrieval_eval/queries.json";
    fs::path output_path    = "tests/retrieval_eval/results.md";
    fs::path baseline_path  = "tests/retrieval_eval/baseline.json";
    fs::path curate_from;     // empty unless --curate-from passed
    int      top_k          = 10;
    double   tolerance      = 0.05;
    bool     verbose        = false;
    bool     no_wait        = false;     // skip waiting for embedding queue
    bool     write_baseline = false;     // --write-baseline overwrites baseline.json
};

void print_help()
{
    std::cout <<
        "locus_retrieval_eval — measure retrieval quality on a workspace.\n"
        "\n"
        "Options:\n"
        "  --workspace <dir>         Workspace to open (default: cwd)\n"
        "  --queries <path>          Gold queries JSON (default: tests/retrieval_eval/queries.json)\n"
        "  --out <path>              Markdown report path (default: tests/retrieval_eval/results.md)\n"
        "  --baseline <path>         Baseline JSON path (default: tests/retrieval_eval/baseline.json)\n"
        "  --top-k <N>               Per-method retrieval depth (default: 10)\n"
        "  --tolerance <f>           Max allowed metric drop (default: 0.05 = 5%)\n"
        "  --write-baseline          Overwrite the baseline JSON with the current run\n"
        "  --curate-from <dir>       Extract search queries from sessions dir, print as JSON, exit\n"
        "  --no-wait                 Don't wait for the embedding queue to drain\n"
        "  --verbose                 spdlog at trace level\n"
        "  --help                    Show this message\n";
}

bool parse_args(int argc, char** argv, Args& a)
{
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto needs = [&](const char* n) {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << n << "\n";
                return false;
            }
            return true;
        };
        if (s == "--workspace")     { if (!needs("--workspace")) return false; a.workspace      = argv[++i]; }
        else if (s == "--queries")  { if (!needs("--queries"))  return false; a.queries_path   = argv[++i]; }
        else if (s == "--out")      { if (!needs("--out"))      return false; a.output_path    = argv[++i]; }
        else if (s == "--baseline") { if (!needs("--baseline")) return false; a.baseline_path  = argv[++i]; }
        else if (s == "--top-k")    { if (!needs("--top-k"))    return false; a.top_k          = std::stoi(argv[++i]); }
        else if (s == "--tolerance"){ if (!needs("--tolerance"))return false; a.tolerance      = std::stod(argv[++i]); }
        else if (s == "--curate-from") { if (!needs("--curate-from")) return false; a.curate_from = argv[++i]; }
        else if (s == "--no-wait")          a.no_wait = true;
        else if (s == "--write-baseline")   a.write_baseline = true;
        else if (s == "--verbose" || s == "-v") a.verbose = true;
        else if (s == "--help" || s == "-h") { print_help(); std::exit(0); }
        else {
            std::cerr << "Unknown argument: " << s << "\n";
            print_help();
            return false;
        }
    }
    return true;
}

// Wait for the embedding worker queue to drain so semantic search has full
// coverage. Cap wall clock so a stuck worker doesn't hang the harness.
void wait_for_embeddings(EmbeddingWorker& worker, int max_seconds)
{
    auto start = std::chrono::steady_clock::now();
    int last_done = -1;
    int silent_seconds = 0;
    while (true) {
        auto s = worker.stats();
        if (!s.active && s.done >= s.total) {
            spdlog::info("Embedding queue drained: {}/{}", s.done, s.total);
            return;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > max_seconds) {
            spdlog::warn("Embedding queue still active after {}s ({}/{}); proceeding "
                         "with partial coverage", max_seconds, s.done, s.total);
            return;
        }
        if (s.done == last_done) {
            ++silent_seconds;
            if (silent_seconds > 30) {  // no progress for 30s — likely idle
                spdlog::warn("Embedding queue made no progress in 30s ({}/{}); "
                             "proceeding", s.done, s.total);
                return;
            }
        } else {
            silent_seconds = 0;
            last_done = s.done;
        }
        if (elapsed % 5 == 0) {
            spdlog::info("Waiting for embeddings: {}/{}", s.done, s.total);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace

int main(int argc, char** argv) try
{
    Args args;
    if (!parse_args(argc, argv, args)) return 2;

    auto console = spdlog::stdout_color_mt("eval");
    spdlog::set_default_logger(console);
    spdlog::set_pattern("[%^%l%$] %v");
    spdlog::set_level(args.verbose ? spdlog::level::trace : spdlog::level::info);

    args.workspace = fs::absolute(args.workspace);

    // -- --curate-from — short-circuit, no workspace needed -------------------
    if (!args.curate_from.empty()) {
        auto curated = eval::curate_from_sessions(args.curate_from);
        std::cout << eval::to_queries_json_template(curated) << "\n";
        return 0;
    }

    if (!fs::is_directory(args.workspace)) {
        spdlog::error("Workspace not a directory: {}", args.workspace.string());
        return 2;
    }

    spdlog::info("Loading queries from {}", args.queries_path.string());
    std::vector<eval::GoldQuery> queries;
    try {
        queries = eval::load_queries(args.queries_path);
    } catch (const std::exception& e) {
        spdlog::error("Failed to load queries: {}", e.what());
        return 2;
    }
    spdlog::info("Loaded {} gold queries", queries.size());

    spdlog::info("Opening workspace: {}", args.workspace.string());
    Workspace ws(args.workspace);

    // Wait for embeddings — semantic / hybrid recall is meaningless without
    // them. The initial index is synchronous (build_initial finished inside
    // the Workspace ctor), but the embedding worker is async.
    if (auto* w = ws.embedding_worker(); w && !args.no_wait) {
        wait_for_embeddings(*w, /*max_seconds=*/600);
    }

    const std::vector<int> ks = {1, 3, 5, 10};
    const int top_k = std::max(args.top_k, 10);

    auto run = eval::run_all(ws, queries, ks, top_k);

    // -- Markdown report ------------------------------------------------------
    std::string md = eval::format_markdown(run, args.workspace.string());
    {
        std::error_code ec;
        fs::create_directories(args.output_path.parent_path(), ec);
        std::ofstream out(args.output_path);
        if (!out.is_open()) {
            spdlog::error("Cannot open output: {}", args.output_path.string());
            return 2;
        }
        out << md;
    }
    spdlog::info("Report written to {}", args.output_path.string());

    // -- Console summary ------------------------------------------------------
    auto print_method = [&](const char* name, const eval::PerMethodResult& m) {
        if (!m.enabled) {
            std::cout << "  " << name << ": disabled (" << m.disabled_reason << ")\n";
            return;
        }
        std::cout << "  " << name
                  << "  R@1=" << m.aggregate.mean_recall_at_k[0]
                  << "  R@3=" << m.aggregate.mean_recall_at_k[1]
                  << "  R@10=" << m.aggregate.mean_recall_at_k[3]
                  << "  MRR="  << m.aggregate.mean_mrr
                  << "  nDCG@10=" << m.aggregate.mean_ndcg_at_10
                  << "  hit=" << m.aggregate.queries_any_hit << "/" << m.aggregate.queries
                  << "  (" << m.wall_ms << " ms)\n";
    };
    std::cout << "\nResults (" << run.queries.size() << " queries):\n";
    print_method("text     ", run.text);
    print_method("semantic ", run.semantic);
    print_method("hybrid   ", run.hybrid);
    std::cout << "\n";

    // -- Baseline / regression ------------------------------------------------
    std::string current_baseline = eval::format_baseline_json(run);

    if (args.write_baseline) {
        std::ofstream out(args.baseline_path);
        out << current_baseline;
        spdlog::info("Baseline written to {}", args.baseline_path.string());
        return 0;
    }

    std::string baseline_text;
    if (fs::exists(args.baseline_path)) {
        std::ifstream b(args.baseline_path);
        std::stringstream ss;
        ss << b.rdbuf();
        baseline_text = ss.str();
    } else {
        spdlog::info("No baseline at {} — first run; rerun with --write-baseline "
                     "to commit one", args.baseline_path.string());
        return 0;
    }

    auto regs = eval::detect_regressions(baseline_text, run, args.tolerance);
    if (regs.empty()) {
        spdlog::info("No regressions vs. baseline (tolerance {}%)",
                     args.tolerance * 100.0);
        return 0;
    }

    std::cerr << "\nRegressions vs. baseline (tolerance "
              << (args.tolerance * 100.0) << "%):\n";
    for (const auto& r : regs) {
        std::cerr << "  " << r.method << " " << r.metric
                  << ": " << r.baseline << " -> " << r.current << "\n";
    }
    return 1;
}
catch (const std::exception& e) {
    spdlog::error("Eval crashed: {}", e.what());
    return 2;
}
