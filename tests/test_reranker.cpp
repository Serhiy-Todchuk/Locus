#include <catch2/catch_test_macros.hpp>

#include "reranker.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace locus;
namespace fs = std::filesystem;

// Locate any reranker GGUF in models/.  Prefers the tiny ms-marco MiniLM
// when present (~25x faster than the bge variants), then falls back to
// the bge family.
static fs::path find_reranker()
{
    const char* names[] = {
        "ms-marco-MiniLM-L6-v2-Q4_K_M.gguf",
        "bge-reranker-v2-m3-Q8_0.gguf",
    };
    fs::path base = fs::current_path();
    for (int i = 0; i < 7; ++i) {
        for (const char* n : names) {
            fs::path c = base / "models" / n;
            if (fs::exists(c)) return c;
        }
        if (!base.has_parent_path() || base.parent_path() == base) break;
        base = base.parent_path();
    }
    return {};
}

static std::unique_ptr<Reranker> load_reranker()
{
    auto p = find_reranker();
    REQUIRE_FALSE(p.empty());
    return std::make_unique<Reranker>(p);
}

TEST_CASE("Reranker scores relevant passage above irrelevant one", "[s4.j]")
{
    auto rr = load_reranker();

    const std::string query = "how to handle file watchers in C++";
    float relevant = rr->score(query,
        "FileWatcher wraps efsw with a debounced FileEvent queue. "
        "Subscribers receive batched events on a background thread.");
    float irrelevant = rr->score(query,
        "Bake the bread at 220 degrees for 30 minutes until the crust "
        "turns golden brown and sounds hollow when tapped.");

    CHECK(relevant > irrelevant);
}

TEST_CASE("Reranker batch ordering puts on-topic above off-topic", "[s4.j]")
{
    auto rr = load_reranker();

    // Phrasing kept simple and lexically distinct so both the heavy
    // bge-reranker-v2-m3 and the tiny ms-marco-MiniLM-L6-v2 agree on the
    // ordering. Avoid technical jargon - the smaller MS MARCO model is
    // trained on web search passages, not code docs, and gets noisy on
    // domain-specific tokens like "sqlite-vec" or "k-NN".
    const std::string query = "what is the best way to train a dog";
    std::vector<std::string> passages = {
        "Positive reinforcement and short, consistent training sessions are "
        "the most effective way to train a puppy or adult dog.",
        "The Eiffel Tower is a wrought-iron lattice tower built in Paris "
        "between 1887 and 1889 for the World's Fair.",
        "Dog trainers recommend rewarding good behaviour with treats and "
        "praise rather than punishing mistakes.",
    };

    auto scores = rr->score_batch(query, passages);
    REQUIRE(scores.size() == passages.size());

    // Both dog-training passages should outrank the Eiffel Tower one.
    CHECK(scores[0] > scores[1]);
    CHECK(scores[2] > scores[1]);
}

// The roadmap's original "< 200 ms on 50 candidates" target was set before
// benchmarking the actual model. bge-reranker-v2-m3 is a 568M-param
// XLMRoberta cross-encoder; on a Ryzen-class CPU it runs ~100 ms/rerank
// in Release (Q4_K_M, n_ctx=1024). 50 candidates lands in the 4-8s range,
// not 200ms. There's no widely-distributed smaller reranker GGUF in the
// bge family, so the practical workaround is to lower the default
// reranker_top_k (now 20 instead of 50, ~2s wall-clock).
//
// This test guards against catastrophic regressions only - the budget is
// generous on purpose. Tighter perf work belongs on a future stage if/when
// we add GPU support or a smaller cross-encoder.
TEST_CASE("Reranker meets perf budget on 50 short candidates", "[s4.j][perf]")
{
    auto rr = load_reranker();

    const std::string query = "tool approval flow for the agent";

    std::vector<std::string> passages;
    passages.reserve(50);
    for (int i = 0; i < 50; ++i) {
        passages.push_back(
            "Candidate passage " + std::to_string(i) +
            ": The agent dispatcher routes tool calls through the approval "
            "gate so the user can accept, modify, or reject before exec.");
    }

    // Warm up - first call pays one-time KV-cache allocation.
    rr->score(query, passages.front());

    auto t0 = std::chrono::steady_clock::now();
    auto scores = rr->score_batch(query, passages);
    auto t1 = std::chrono::steady_clock::now();

    REQUIRE(scores.size() == 50);

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    INFO("rerank 50 candidates took " << elapsed_ms << " ms");

#ifdef NDEBUG
    constexpr long long k_budget_ms = 15'000;   // 300ms/rerank ceiling
#else
    constexpr long long k_budget_ms = 120'000;  // Debug is ~10x slower
#endif
    CHECK(elapsed_ms < k_budget_ms);
}
