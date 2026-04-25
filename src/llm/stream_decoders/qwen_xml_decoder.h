#pragma once

#include "llm/stream_decoder.h"

#include <memory>

namespace locus {

// Decodes streaming responses from models that emit tool calls as
//   <tool_call>{"name":"...","arguments":{...}}</tool_call>
// in the content channel. Wraps an OpenAiDecoder for the SSE chunk
// shape and a XmlToolCallExtractor for the embedded markers, so any
// native JSON `tool_calls` deltas the server emits are still passed
// through unchanged. Stream-stateful: reset() between streams.
class QwenXmlDecoder : public IStreamDecoder {
public:
    QwenXmlDecoder();
    ~QwenXmlDecoder() override;

    bool decode(const std::string& payload, const StreamDecoderSink& sink) override;
    void finish_stream(const StreamDecoderSink& sink) override;
    void reset() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace locus
