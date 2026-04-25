#pragma once

#include "llm/llm_client.h"   // CompletionUsage

#include <functional>
#include <string>

namespace locus {

// Sink for typed events emitted by an IStreamDecoder. The sink lives on
// the caller's stack -- decoders never store callbacks across calls.
struct StreamDecoderSink {
    // Visible answer token.
    std::function<void(const std::string& token)> on_text;

    // Chain-of-thought / reasoning channel (LM Studio's reasoning_content,
    // Claude's <thinking>, Qwen's <think>, ...). Empty = ignored.
    std::function<void(const std::string& token)> on_reasoning;

    // One slice of a streamed tool call. Caller appends `name_frag` /
    // `args_frag` per index across the run; `id_frag` (if non-empty)
    // overwrites the slot's id (typically delivered only on the first
    // slice of each call).
    struct ToolCallDelta {
        int         index = 0;
        std::string id_frag;
        std::string name_frag;
        std::string args_frag;
    };
    std::function<void(const ToolCallDelta& delta)> on_tool_call_delta;

    // Server-reported token usage (typically arrives in the final chunk).
    std::function<void(const CompletionUsage& usage)> on_usage;
};

// Translates one SSE data payload into typed events. Decoders may keep
// internal state across calls within a single stream (XML formats need
// this); callers get a fresh decoder per stream or call `reset()`.
class IStreamDecoder {
public:
    virtual ~IStreamDecoder() = default;

    // Decode one SSE `data:` payload. The transport handles the [DONE]
    // sentinel before calling decode(), so payload is always real data.
    // Returns false to abort the stream early.
    virtual bool decode(const std::string& payload, const StreamDecoderSink& sink) = 0;

    // Stream is closing -- emit any text or tool-call deltas the decoder
    // was holding back waiting for more bytes (XML decoders that buffer
    // a few bytes to disambiguate partial-tag prefixes). Called once
    // after the transport finishes, before the client emits its own
    // on_complete. No-op for stateless decoders (OpenAI).
    virtual void finish_stream(const StreamDecoderSink& /*sink*/) {}

    // Reset any per-stream accumulator state. No-op for stateless
    // decoders (OpenAI). XML decoders flush partial-tag buffers here.
    virtual void reset() {}
};

} // namespace locus
