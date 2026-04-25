#pragma once

#include "llm/llm_client.h"   // LLMConfig

#include <functional>
#include <string>

namespace locus {

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
