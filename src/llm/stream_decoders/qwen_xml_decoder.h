#pragma once

#include "llm/stream_decoder.h"

namespace locus {

// Stub for Qwen-style streaming where tool calls are embedded as
//   <tool_call>{"name":"...","arguments":{...}}</tool_call>
// in the content channel. S4.N implements this; today the call site
// fails loudly so the router cannot accidentally select it.
class QwenXmlDecoder : public IStreamDecoder {
public:
    bool decode(const std::string& payload, const StreamDecoderSink& sink) override;
};

} // namespace locus
