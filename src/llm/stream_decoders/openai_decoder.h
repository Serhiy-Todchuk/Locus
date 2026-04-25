#pragma once

#include "llm/stream_decoder.h"

namespace locus {

// Decodes OpenAI-compatible chat completion SSE chunks of the form
//   {"choices":[{"delta":{"content":"...","reasoning_content":"...",
//                          "tool_calls":[{...}]}}], "usage":{...}}
// Stateless across calls -- every payload is a complete JSON object.
class OpenAiDecoder : public IStreamDecoder {
public:
    bool decode(const std::string& payload, const StreamDecoderSink& sink) override;
};

} // namespace locus
