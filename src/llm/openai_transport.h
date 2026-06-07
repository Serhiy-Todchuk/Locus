#pragma once

#include "llm/llm_client.h"   // LLMConfig

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace locus {

// S6.16 -- builds the request header list from config: always Content-Type +
// Accept; appends `Authorization: Bearer <api_key>` when the key is non-empty;
// then merges every `extra_headers` entry verbatim. Extracted as a free
// function so it can be unit-tested without a live cpr::Post. The API key is
// returned verbatim in the Authorization value but is never logged by the
// transport. Returns an ordered list of {name, value} pairs.
std::vector<std::pair<std::string, std::string>>
build_request_headers(const LLMConfig& config);

// Transient-failure retry policy (2026-06-06). Hosted free-tier endpoints
// (NVIDIA Build, OpenRouter free models, ...) intermittently rate-limit
// (HTTP 429), drop the connection after a long queue wait (stall/timeout),
// or return a clean HTTP 200 with an EMPTY body / zero SSE chunks. Before
// this, any of the three killed the entire agent turn with no retry, losing
// all progress. `classify_retry` is the pure decision: given the observed
// outcome of one attempt, should we retry, and after how long a backoff?
//
// Retryable (transient):
//   - cpr OPERATION_TIMEDOUT (stream stalled / never connected past the wait)
//   - HTTP 429 (rate limited) and HTTP 5xx (server-side hiccup)
//   - HTTP 200 with zero chunks received (empty body / silent drop)
// NOT retryable (deterministic): HTTP 4xx other than 429 (bad request, auth),
//   any attempt where the caller asked to cancel, or once `attempt` has
//   reached `max_attempts`.
//
// Backoff is exponential off `base_backoff_ms`, capped, with the floor raised
// to the server's `Retry-After` when present (429s). Kept as a free function
// so the matrix is unit-tested without a live endpoint.
struct RetryDecision {
    bool        retry = false;
    long long   backoff_ms = 0;
    std::string reason;       // for the log line; empty when retry == false
};

RetryDecision classify_retry(int          cpr_error_code,   // cpr::ErrorCode as int
                             long         http_status,      // 0 when transport failed
                             uint64_t     chunks_received,
                             bool         cancelled,
                             int          attempt,          // 0-based: this attempt's index
                             int          max_attempts,
                             long long    retry_after_ms,   // <0 if header absent
                             long long    base_backoff_ms = 2000,
                             long long    max_backoff_ms   = 20000);

// Wraps cpr POST /v1/chat/completions + SSE parsing. Knows nothing
// about JSON: hands each `data:` payload to the on_data callback as a
// raw string, swallows the [DONE] sentinel, and handles HTTP / connect
// / stall errors (including a one-shot stall retry).
//
// Higher layers compose Transport + an IStreamDecoder to translate the
// raw SSE stream into typed events (text / reasoning / tool-call / usage).
class OpenAiTransport {
public:
    struct Callbacks {
        // Receives each SSE data payload (post-[DONE] check). Return
        // false to abort the stream early.
        std::function<bool(const std::string& sse_data)> on_data;
        // Transport / HTTP / connect / stall error.
        std::function<void(const std::string& err)> on_error;
        // Polled by the cpr write callback before each chunk is parsed. If
        // it returns true the HTTP transfer aborts immediately and post_chat
        // returns *without* firing on_error -- the caller is expected to
        // know about the cancellation.
        std::function<bool()> should_cancel;
    };

    explicit OpenAiTransport(const LLMConfig& config);

    // Send a chat completion request. body is the full JSON payload
    // ({"messages":[...], "tools":[...], "stream":true, ...}). Blocks
    // until the response is fully consumed or an error fires.
    void post_chat(const std::string& body, const Callbacks& cbs);

private:
    // `attempt` is the 0-based index of this attempt; do_post re-invokes
    // itself with attempt+1 after a backoff when classify_retry says so.
    void do_post(const std::string& body, const Callbacks& cbs, int attempt);

    const LLMConfig& config_;

    // Max total attempts per request (1 original + N-1 retries). Transient
    // hosted-endpoint failures are retried up to this many times. 5 gives a
    // sustained free-tier 429 window (~30-60s) enough backoff cycles to clear:
    // with the 8s 429 floor the waits are ~8+16+20+20s before the final fail.
    static constexpr int k_max_attempts = 5;
};

} // namespace locus
