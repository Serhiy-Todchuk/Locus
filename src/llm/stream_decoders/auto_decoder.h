#pragma once

#include "llm/stream_decoder.h"

#include <memory>

namespace locus {

// Default decoder when the user hasn't pinned a tool format. Composes
// an OpenAiDecoder for SSE chunk shape with an XmlToolCallExtractor
// watching BOTH Qwen (`<tool_call>`) and Claude (`<function_calls>`)
// markers. Native JSON `tool_calls` deltas pass through unchanged.
//
// Once an XML tool call fires, the extractor's internal state ensures
// any second-format markers in subsequent text are still detected
// (cheap: a few bytes of buffering). This means a heuristic "model
// emits Qwen format" lock-in is implicit -- we never reject a future
// Claude marker, but in practice models pick one format and stick.
class AutoToolFormatDecoder : public IStreamDecoder {
public:
    AutoToolFormatDecoder();
    ~AutoToolFormatDecoder() override;

    bool decode(const std::string& payload, const StreamDecoderSink& sink) override;
    void finish_stream(const StreamDecoderSink& sink) override;
    void reset() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace locus
