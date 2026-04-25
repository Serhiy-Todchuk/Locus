#include "llm/stream_decoders/qwen_xml_decoder.h"

#include <stdexcept>

namespace locus {

bool QwenXmlDecoder::decode(const std::string& /*payload*/,
                            const StreamDecoderSink& /*sink*/)
{
    throw std::logic_error(
        "QwenXmlDecoder not implemented yet (planned for S4.N)");
}

} // namespace locus
