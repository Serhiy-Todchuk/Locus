#include "llm/openai_transport.h"
#include "llm/sse_parser.h"

#include <cpr/cpr.h>
#include <spdlog/spdlog.h>

#include <algorithm>

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

    // Use cpr WriteCallback to feed the parser incrementally. Polling the
    // cancel predicate here (rather than only between SSE events) means a
    // Stop click is honoured within one HTTP read -- typically a few hundred
    // bytes or less of further output, not the rest of the response.
    cpr::WriteCallback write_cb{[&](std::string_view data,
                                    intptr_t /*userdata*/) -> bool {
        if (cbs.should_cancel && cbs.should_cancel()) {
            // Returning false makes libcurl abort the transfer with
            // CURLE_WRITE_ERROR; OpenAiTransport::do_post detects that
            // below via cbs.should_cancel() and treats it as clean exit.
            return false;
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
                spdlog::warn("LLM stream stalled (no data for {}s), retrying once...",
                             stall_seconds);
                do_post(body, cbs, true);
                return;
            }
            err_msg = "LLM stream stalled after retry (no data for " +
                      std::to_string(stall_seconds) + "s)";
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
