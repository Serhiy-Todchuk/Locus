#include "llm/openai_transport.h"
#include "llm/sse_parser.h"

#include <cpr/cpr.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace locus {

RetryDecision classify_retry(int       cpr_error_code,
                             long      http_status,
                             uint64_t  chunks_received,
                             bool      cancelled,
                             int       attempt,
                             int       max_attempts,
                             long long retry_after_ms,
                             long long base_backoff_ms,
                             long long max_backoff_ms)
{
    RetryDecision d;

    // Never retry a user-cancelled call, and never exceed the attempt budget.
    if (cancelled) return d;
    if (attempt + 1 >= max_attempts) return d;

    const bool transport_ok =
        cpr_error_code == static_cast<int>(cpr::ErrorCode::OK);
    const bool timed_out =
        cpr_error_code == static_cast<int>(cpr::ErrorCode::OPERATION_TIMEDOUT);

    bool transient     = false;
    bool is_rate_limit = false;
    if (timed_out) {
        d.reason  = "stream stalled / timed out";
        transient = true;
    } else if (transport_ok && http_status == 429) {
        d.reason      = "HTTP 429 (rate limited)";
        transient     = true;
        is_rate_limit = true;
    } else if (transport_ok && http_status >= 500 && http_status < 600) {
        d.reason  = "HTTP " + std::to_string(http_status) + " (server error)";
        transient = true;
    } else if (transport_ok && http_status == 200 && chunks_received == 0) {
        d.reason  = "empty response (HTTP 200, zero chunks)";
        transient = true;
    }
    // Deterministic failures (connect refused, 4xx other than 429, auth) are
    // NOT retried -- a retry would just reproduce them.

    if (!transient) return d;

    // Exponential backoff off base, capped. Parenthesise std::min to dodge the
    // windows.h min() macro.
    //
    // Rate-limit (429) windows on free hosted tiers (NVIDIA Build, OpenRouter
    // free) last much longer than a server hiccup or an empty-body drop -- 3
    // quick 2s/4s retries observed exhausting against a sustained 429 window in
    // the 2026-06-06 session. So 429s without a Retry-After get a higher floor
    // so the backoff actually outlasts a typical rate-limit window.
    long long effective_base =
        is_rate_limit ? (std::max)(base_backoff_ms, 8000LL) : base_backoff_ms;
    long long backoff = effective_base;
    for (int i = 0; i < attempt; ++i)
        backoff = (std::min)(max_backoff_ms, backoff * 2);
    if (retry_after_ms > backoff)
        backoff = (std::min)(retry_after_ms, max_backoff_ms * 3);  // honour 429 hint

    d.retry      = true;
    d.backoff_ms = backoff;
    return d;
}

std::vector<std::pair<std::string, std::string>>
build_request_headers(const LLMConfig& config)
{
    std::vector<std::pair<std::string, std::string>> h;
    h.emplace_back("Content-Type", "application/json");
    h.emplace_back("Accept",       "text/event-stream");
    if (!config.api_key.empty())
        h.emplace_back("Authorization", "Bearer " + config.api_key);
    for (const auto& [k, v] : config.extra_headers)
        h.emplace_back(k, v);
    return h;
}

OpenAiTransport::OpenAiTransport(const LLMConfig& config)
    : config_(config)
{
}

void OpenAiTransport::post_chat(const std::string& body, const Callbacks& cbs)
{
    do_post(body, cbs, 0);
}

void OpenAiTransport::do_post(const std::string& body, const Callbacks& cbs, int attempt)
{
    std::string url = join_api_url(config_.base_url, "/v1/chat/completions");

    spdlog::trace("LLM POST {}", url);

    // SSE parser feeds on raw response bytes. Swallow [DONE] here so
    // decoders never have to special-case the sentinel.
    SseParser parser([&](const std::string& data) -> bool {
        if (data == "[DONE]") {
            spdlog::trace("LLM stream: [DONE]");
            return false;  // stop parsing
        }
        if (cbs.on_data)
            return cbs.on_data(data);
        return true;
    });

    // Per-stream pacing instrumentation. Lets the agentic post-mortem tell
    // "model stalled" (zero bytes for many seconds) apart from "model slow"
    // (steady trickle of small chunks). Time-to-first-byte logged at info;
    // long inter-chunk gaps logged at warn; periodic alive heartbeat logged
    // at info every 32 chunks OR every 10s (whichever first) so the
    // `--agentic-mute-noise` flush-on-info threshold lets agentic clients
    // see "stream still alive" without trace-level chatter.
    //
    // Why both? Fast streams (4+ chunks/s) get the 32-chunk cadence (~8s).
    // Slow reasoning streams (one chunk every few seconds) get the 10s
    // wall-clock cadence -- without it a stream emitting one chunk every
    // 20s would look stuck to wait_for_agent_idle for 64*20s = ~20 min
    // before its info-level beacon arrived.
    const auto t_request_start = std::chrono::steady_clock::now();
    auto t_last_chunk     = t_request_start;
    auto t_last_heartbeat = t_request_start;
    bool first_chunk_seen = false;
    uint64_t total_bytes = 0;
    uint64_t chunks = 0;
    long long max_gap_ms = 0;

    cpr::WriteCallback write_cb{[&](std::string_view data,
                                    intptr_t /*userdata*/) -> bool {
        if (cbs.should_cancel && cbs.should_cancel()) {
            // Returning false makes libcurl abort the transfer with
            // CURLE_WRITE_ERROR; OpenAiTransport::do_post detects that
            // below via cbs.should_cancel() and treats it as clean exit.
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        const long long gap_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - t_last_chunk).count();
        if (!first_chunk_seen) {
            first_chunk_seen = true;
            t_last_heartbeat = now;
            const long long ttfb_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - t_request_start).count();
            spdlog::info("LLM stream: first chunk after {} ms ({} bytes)",
                         ttfb_ms, data.size());
        } else if (gap_ms > 60000) {
            // Quiet stretch longer than 60 s -- useful early warning before
            // the LowSpeed watchdog fires (which only triggers at config_.timeout_ms,
            // typically 1800 s for local-LLM setups).
            spdlog::warn("LLM stream: inter-chunk gap {} ms ({} bytes so far, {} chunks)",
                         gap_ms, total_bytes, chunks);
        }
        if (gap_ms > max_gap_ms) max_gap_ms = gap_ms;
        t_last_chunk = now;
        ++chunks;
        total_bytes += data.size();
        // Heartbeat: every 32 chunks OR every 10s since last emit. Info-level
        // so agentic mute lets it through to .locus/locus.log immediately
        // (trace lines hang in the buffer for tens of minutes when flush_on
        // is info, which is exactly what TestLocalVibe finding #1 hit).
        const long long since_hb_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - t_last_heartbeat).count();
        if ((chunks & 31) == 0 || since_hb_ms >= 10000) {
            spdlog::info("LLM stream: chunk #{} (+{} B, gap {} ms, total {} B)",
                         chunks, data.size(), gap_ms, total_bytes);
            t_last_heartbeat = now;
        }
        parser.feed(std::string(data));
        return true;
    }};

    // Stall-based timeout, not total-transfer cap: thinking models can
    // emit reasoning tokens for minutes without exceeding the old
    // cpr::Timeout. LowSpeed aborts only if fewer than 1 B/s flows for
    // timeout_ms -- i.e. the stream truly stopped.
    const long stall_seconds =
        std::max<long>(1, static_cast<long>(config_.timeout_ms) / 1000);

    // S6.16 -- header block from config (Bearer auth + extra_headers). The
    // API key flows into the Authorization value but is never logged: the
    // only trace line above prints the URL, not the headers.
    cpr::Header headers;
    for (auto& [k, v] : build_request_headers(config_))
        headers[k] = v;

    cpr::Response response = cpr::Post(
        cpr::Url{url},
        headers,
        cpr::Body{body},
        cpr::ConnectTimeout{10000},
        cpr::LowSpeed{1, stall_seconds},
        std::move(write_cb)
    );

    // Flush any remaining data in the SSE buffer.
    parser.finish();

    // End-of-stream pacing summary. Logged regardless of success/failure so
    // a stalled call still surfaces what it managed to receive.
    {
        const auto t_end = std::chrono::steady_clock::now();
        const long long total_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                t_end - t_request_start).count();
        spdlog::info(
            "LLM stream: end ({} chunks, {} bytes, total {} ms, max inter-chunk gap {} ms{})",
            chunks, total_bytes, total_ms, max_gap_ms,
            first_chunk_seen ? "" : ", NO chunks received");
    }

    // User-requested cancellation -- treat any transport error (libcurl
    // surfaces it as a write error when the WriteCallback returns false) as
    // a clean exit. Skips the stall-retry path and suppresses on_error so
    // the chat doesn't show "Turn cancelled" twice (once here, once from
    // AgentCore).
    const bool cancelled = cbs.should_cancel && cbs.should_cancel();
    if (cancelled) {
        spdlog::info("LLM stream aborted by user cancellation");
        return;
    }

    // Parse a Retry-After header (seconds, or an HTTP-date we ignore) so a
    // 429 backoff honours the server's stated cooldown floor.
    long long retry_after_ms = -1;
    {
        auto it = response.header.find("Retry-After");
        if (it == response.header.end()) it = response.header.find("retry-after");
        if (it != response.header.end()) {
            try {
                long secs = std::stol(it->second);
                if (secs > 0) retry_after_ms = static_cast<long long>(secs) * 1000;
            } catch (...) { /* HTTP-date form -- ignore, fall back to backoff */ }
        }
    }

    // Unified transient-failure retry: timeout, 429, 5xx, and empty-200 all
    // route through the same pure decision. Hosted free-tier endpoints hit all
    // four; before this, each killed the whole agent turn with no retry.
    RetryDecision rd = classify_retry(
        static_cast<int>(response.error.code),
        response.status_code,
        chunks,
        cancelled,
        attempt,
        k_max_attempts,
        retry_after_ms);

    if (rd.retry) {
        spdlog::warn(
            "LLM request transient failure ({}); attempt {}/{}, retrying after "
            "{} ms backoff. Received {} chunks / {} bytes this attempt.",
            rd.reason, attempt + 1, k_max_attempts, rd.backoff_ms,
            chunks, total_bytes);
        if (cbs.on_status) {
            long secs = (rd.backoff_ms + 999) / 1000;
            cbs.on_status(rd.reason + " -- retrying " +
                          std::to_string(attempt + 2) + "/" +
                          std::to_string(k_max_attempts) + " in " +
                          std::to_string(secs) + "s");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(rd.backoff_ms));
        // Re-check cancellation after the backoff -- the user may have hit Stop
        // while we waited.
        if (cbs.should_cancel && cbs.should_cancel()) {
            spdlog::info("LLM retry aborted by user cancellation during backoff");
            return;
        }
        do_post(body, cbs, attempt + 1);
        return;
    }

    // Transport-level errors (non-retryable, or retries exhausted).
    if (response.error.code != cpr::ErrorCode::OK) {
        std::string err_msg;
        if (response.error.code == cpr::ErrorCode::COULDNT_CONNECT) {
            err_msg = "Cannot connect to LLM server at " + config_.base_url +
                      " - is LM Studio running?";
        } else if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
            // Build a user-visible error that names the observed progress AND
            // points at the fix. The chat-bubble surfaces this verbatim, so
            // every word is part of the UX.
            err_msg = "LLM stream stalled after " + std::to_string(attempt + 1) +
                      " attempt(s) (no data for " +
                      std::to_string(stall_seconds) + "s). " +
                      "Received " + std::to_string(chunks) + " chunks / " +
                      std::to_string(total_bytes) + " bytes during the last attempt"
                      + (first_chunk_seen
                            ? "; the model started responding but then went silent."
                            : "; the model never sent a first byte.") +
                      " If this is a slow local model, raise llm.timeout_ms in "
                      ".locus/config.json (Settings -> LLM -> Stream stall timeout).";
        } else {
            err_msg = "LLM request failed: " + response.error.message;
        }

        spdlog::error("{}", err_msg);
        if (cbs.on_error)
            cbs.on_error(err_msg);
        return;
    }

    // HTTP status (non-retryable, or retries exhausted).
    if (response.status_code != 200) {
        std::string err_msg = "LLM server returned HTTP " +
                              std::to_string(response.status_code);
        if (response.status_code == 429)
            err_msg += " (rate limited) after " +
                       std::to_string(attempt + 1) + " attempt(s)";
        if (!response.text.empty())
            err_msg += ": " + response.text.substr(0, 500);

        spdlog::error("{}", err_msg);
        if (cbs.on_error)
            cbs.on_error(err_msg);
        return;
    }
}

} // namespace locus
