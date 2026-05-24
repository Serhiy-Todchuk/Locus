#include "llm/openai_transport.h"
#include "llm/sse_parser.h"

#include <cpr/cpr.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace locus {

OpenAiTransport::OpenAiTransport(const LLMConfig& config)
    : config_(config)
{
}

void OpenAiTransport::post_chat(const std::string& body, const Callbacks& cbs)
{
    do_post(body, cbs, false);
}

void OpenAiTransport::do_post(const std::string& body, const Callbacks& cbs, bool is_retry)
{
    std::string url = config_.base_url + "/v1/chat/completions";

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

    cpr::Response response = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Content-Type", "application/json"},
            {"Accept",       "text/event-stream"}
        },
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
    if (cbs.should_cancel && cbs.should_cancel()) {
        spdlog::info("LLM stream aborted by user cancellation");
        return;
    }

    // Transport-level errors.
    if (response.error.code != cpr::ErrorCode::OK) {
        std::string err_msg;
        if (response.error.code == cpr::ErrorCode::COULDNT_CONNECT) {
            err_msg = "Cannot connect to LLM server at " + config_.base_url +
                      " - is LM Studio running?";
        } else if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
            if (!is_retry) {
                spdlog::warn(
                    "LLM stream stalled (no data for {}s, received {} chunks / {} bytes "
                    "before stall), retrying once. If this keeps happening on a slow "
                    "local model, raise llm.timeout_ms in .locus/config.json (Settings -> "
                    "LLM -> Stream stall timeout).",
                    stall_seconds, chunks, total_bytes);
                do_post(body, cbs, true);
                return;
            }
            // Build a user-visible error that names the observed progress AND
            // points at the fix. The chat-bubble surfaces this verbatim, so
            // every word is part of the UX.
            err_msg = "LLM stream stalled after retry (no data for " +
                      std::to_string(stall_seconds) + "s). " +
                      "Received " + std::to_string(chunks) + " chunks / " +
                      std::to_string(total_bytes) + " bytes during the retry attempt"
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

    // HTTP status.
    if (response.status_code != 200) {
        std::string err_msg = "LLM server returned HTTP " +
                              std::to_string(response.status_code);
        if (!response.text.empty())
            err_msg += ": " + response.text.substr(0, 500);

        spdlog::error("{}", err_msg);
        if (cbs.on_error)
            cbs.on_error(err_msg);
        return;
    }
}

} // namespace locus
