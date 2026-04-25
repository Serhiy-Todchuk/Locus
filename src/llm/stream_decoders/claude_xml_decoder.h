#pragma once

#include "llm/stream_decoder.h"

namespace locus {

// Stub for Claude-style streaming where tool calls are embedded as
//   <function_calls><invoke name="..."><parameter name="...">...
// in the content channel. S4.N implements this; today the call site
// fails loudly so the router cannot accidentally select it.
class ClaudeXmlDecoder : public IStreamDecoder {
public:
    bool decode(const std::string& payload, const StreamDecoderSink& sink) override;
};

} // namespace locus
