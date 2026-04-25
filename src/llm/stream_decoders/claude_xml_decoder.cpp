#include "llm/stream_decoders/claude_xml_decoder.h"

#include <stdexcept>

namespace locus {

bool ClaudeXmlDecoder::decode(const std::string& /*payload*/,
                              const StreamDecoderSink& /*sink*/)
{
    throw std::logic_error(
        "ClaudeXmlDecoder not implemented yet (planned for S4.N)");
}

} // namespace locus
