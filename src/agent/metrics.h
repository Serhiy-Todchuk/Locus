#pragma once

#include "llm/llm_client.h"

#include <chrono>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace locus {

// Per-turn record. One entry per processed user message; populated as the
// turn progresses. Used directly by the GUI metrics tab to draw spark bars.
struct TurnSample {
    int                                   turn_id      = 0;
    std::chrono::system_clock::time_point started      {};
    std::chrono::system_clock::time_point ended        {};
    long long                             duration_ms  = 0;     // wall-clock for the turn
    long long                             stream_ms    = 0;     // sum of LLM stream wall times
    int                                   prompt_tokens     = 0; // last LLM step's prompt
    int                                   completion_tokens = 0; // sum across rounds
    int                                   reasoning_tokens  = 0; // sum across rounds
    int                                   cached_tokens     = 0; // S4.F: sum across rounds, server-reported
    int                                   total_tokens      = 0; // last server-reported total
    int                                   tokens_delta      = 0; // total minus prev_turn_total
    int                                   llm_rounds        = 0;
    int                                   tool_calls        = 0;
    bool                                  had_error         = false;
    // S4.F -- KV-cache + prefill signals. The hash is std::hash over the
    // outgoing system message (messages[0].content); two adjacent turns with
    // matching hashes mean the cache CAN fire even when the backend doesn't
    // populate cached_tokens. ttft_ms is time-to-first-token of the first LLM
    // round in the turn -- the ground-truth signal that prefill ran short.
    std::size_t                           prompt_prefix_hash = 0;
    long long                             ttft_ms            = 0;
};

// Per-tool aggregate. Counts and latency only — fine-grained per-call detail
// stays in the activity log.
struct ToolStat {
    int       calls    = 0;
    int       failures = 0;
    long long total_ms = 0;
    long long total_chars = 0;  // result content size, summed
};

// Retrieval (search_*) hit-rate aggregate. A "hit" is a query that returned
// at least one row; the result-count-vs-cap ratio is the saturation.
struct RetrievalStat {
    int       queries          = 0;
    int       hits             = 0;
    int       empty            = 0;
    long long results_total    = 0;  // sum of returned row counts
    long long max_results_cap_total = 0;  // sum of max_results values seen
};

// Aggregator: thread-safe, owned by AgentCore. ToolDispatcher and AgentLoop
// hold a reference and call record_*() during a turn; the GUI / exporter read
// snapshots through the json/csv accessors.
//
// All public methods take/return values by copy or by const ref to avoid
// exposing internal mutex state.
class MetricsAggregator {
public:
    MetricsAggregator() = default;

    // -- Recording (called from agent thread) --------------------------------

    // Bracket one user-message turn. begin_turn returns nothing — end_turn
    // closes the most recently opened sample.
    void begin_turn(int turn_id);
    void end_turn(bool had_error);

    // One LLM round trip inside the current turn. `stream_ms` is the wall
    // clock of stream_completion(); `usage` is the server-reported counts.
    void record_llm_step(const CompletionUsage& usage,
                         long long stream_ms,
                         int prev_turn_total);

    // S4.F -- record the system-prompt hash for the current turn. Called once
    // per turn (from AgentLoop's first round); subsequent rounds ignore it
    // to keep the per-turn hash stable. Pass std::hash over messages[0].content.
    void record_prompt_prefix_hash(std::size_t h);

    // S4.F -- record time-to-first-token for the current turn (first round
    // only). Subsequent rounds are ignored. Used to derive a prefill rate
    // (ms / prompt_token) that signals KV cache hits when the backend
    // doesn't populate `prompt_tokens_details.cached_tokens`.
    void record_first_token(long long ttft_ms);

    // One tool invocation. retrieval_count == nullopt for non-search tools.
    void record_tool(const std::string& name,
                     bool ok,
                     long long ms,
                     size_t result_chars,
                     std::optional<int> retrieval_count = std::nullopt,
                     std::optional<int> retrieval_cap   = std::nullopt);

    // -- Reading (any thread) ------------------------------------------------

    // JSON snapshot suitable for embedding in a session file or exporting.
    // Shape:
    //   { "turns":[{...}, ...],
    //     "tools":{ "<name>": {...}, ... },
    //     "retrieval": {...},
    //     "totals": { tokens_in, tokens_out, reasoning_tokens, ... } }
    nlohmann::json to_json() const;

    // CSV: one section per kind of row. Header row + data rows.
    // Sections separated by a blank line. UTF-8.
    std::string to_csv() const;

    // Aggregates used by the GUI tab. Held by value — small POD.
    struct Aggregates {
        int       turn_count       = 0;
        int       tokens_in_total  = 0;   // sum of completion+prompt deltas
        int       tokens_out_total = 0;   // sum of completion_tokens
        int       reasoning_total  = 0;
        long long stream_ms_total  = 0;
        // Tokens/s = sum(completion_tokens) / sum(stream_seconds).
        double    tokens_per_second = 0.0;
        // Wall-clock turn time, summary stats.
        long long avg_turn_ms = 0;
        long long p95_turn_ms = 0;
        long long max_turn_ms = 0;
        // Retrieval hit rate = hits / queries.
        double    retrieval_hit_rate = 0.0;
        int       retrieval_queries  = 0;
        // Tool histogram: name -> calls (sorted descending by callers).
        std::vector<std::pair<std::string, int>> tool_calls_by_name;
        // Per-turn series for spark bars.
        std::vector<long long> turn_durations_ms;
        std::vector<int>       turn_total_tokens;

        // -- S4.F: KV-cache + prefill signals --------------------------------
        // Sum of server-reported cached_tokens across every turn -- a direct
        // signal of cache hits when the backend populates it. Zero on
        // backends that don't report (LM Studio + some llama.cpp versions).
        int       cached_tokens_total   = 0;
        // cached / prompt over the whole session, 0..1. Useful for "is the
        // cache actually doing anything" eyeballing.
        double    cached_token_ratio    = 0.0;
        // True iff the last >=2 turns share the same system-prompt hash --
        // a cheap signal that the cache CAN fire even when the server
        // doesn't surface cached_tokens. False until we've seen two turns.
        bool      prompt_prefix_stable  = false;
        // Per-prompt-token prefill cost from the most recent turn that had
        // both a non-zero TTFT and non-zero prompt_tokens. Drops sharply
        // across consecutive turns with stable prefixes when caching works.
        // Zero when we don't have a usable sample yet.
        double    last_prefill_ms_per_token = 0.0;
        // Time-to-first-token of the most recent turn, in ms.
        long long last_ttft_ms          = 0;
    };

    Aggregates aggregates() const;

    // Snapshot copies for callers that want raw data (e.g. CSV export).
    std::vector<TurnSample>          turns_snapshot()    const;
    std::map<std::string, ToolStat>  tools_snapshot()    const;
    RetrievalStat                    retrieval_snapshot() const;

    // Reset all counters (called on /reset and on session load).
    void reset();

private:
    mutable std::mutex                mutex_;
    std::vector<TurnSample>           turns_;
    std::map<std::string, ToolStat>   tools_;
    RetrievalStat                     retrieval_;
    int                               last_turn_id_ = 0;  // to detect strays
};

// -- Search-result count parser ---------------------------------------------
// Most search tool implementations begin their result with a header like
// "<N> results for ..." or "<N> matches ...". This helper extracts the
// leading integer so MetricsAggregator can record a hit/miss without
// reaching into each tool's formatter.
//
// Returns std::nullopt for non-search tools or unparseable headers.
std::optional<int> parse_search_result_count(const std::string& tool_name,
                                             const std::string& content);

} // namespace locus
