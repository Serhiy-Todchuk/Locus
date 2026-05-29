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
    void do_post(const std::string& body, const Callbacks& cbs, bool is_retry);

    const LLMConfig& config_;
};

} // namespace locus
