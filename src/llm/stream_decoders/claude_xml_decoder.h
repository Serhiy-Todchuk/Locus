#pragma once

#include "llm/stream_decoder.h"

#include <memory>

namespace locus {

// Decodes streaming responses from models that emit tool calls as
//   <function_calls>
//     <invoke name="fn_name">
//       <parameter name="arg1">value1</parameter>
//       ...
//     </invoke>
//   </function_calls>
// in the content channel. Multiple <invoke> blocks per <function_calls>
// are supported. Wraps an OpenAiDecoder + XmlToolCallExtractor so any
// native JSON `tool_calls` deltas still pass through. Stream-stateful.
class ClaudeXmlDecoder : public IStreamDecoder {
public:
    ClaudeXmlDecoder();
    ~ClaudeXmlDecoder() override;

    bool decode(const std::string& payload, const StreamDecoderSink& sink) override;
    void finish_stream(const StreamDecoderSink& sink) override;
    void reset() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace locus
