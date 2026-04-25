#include "llm/stream_decoders/qwen_xml_decoder.h"

#include "llm/stream_decoders/openai_decoder.h"
#include "llm/xml_tool_call_extractor.h"

namespace locus {

// Internal state lives on the heap so the public header (used by the LLM
// router via std::unique_ptr<IStreamDecoder>) doesn't have to drag in
// either of the helper headers above.
struct QwenXmlDecoder::Impl {
    OpenAiDecoder        inner;
    XmlToolCallExtractor extractor;

    Impl() : extractor({XmlMarker::Qwen}) {}
};

QwenXmlDecoder::QwenXmlDecoder() : impl_(std::make_unique<Impl>()) {}
QwenXmlDecoder::~QwenXmlDecoder() = default;

bool QwenXmlDecoder::decode(const std::string& payload,
                            const StreamDecoderSink& sink)
{
    // Forward reasoning / native tool_calls / usage straight through.
    // Intercept text chunks: route them via the XML extractor, which
    // emits filtered text + extracted tool calls back into the outer
    // sink.
    StreamDecoderSink inner_sink;
    inner_sink.on_reasoning       = sink.on_reasoning;
    inner_sink.on_tool_call_delta = sink.on_tool_call_delta;
    inner_sink.on_usage           = sink.on_usage;
    inner_sink.on_text = [&](const std::string& token) {
        impl_->extractor.feed(
            token,
            [&](const std::string& clean) {
                if (sink.on_text && !clean.empty()) sink.on_text(clean);
            },
            [&](const StreamDecoderSink::ToolCallDelta& d) {
                if (sink.on_tool_call_delta) sink.on_tool_call_delta(d);
            }
        );
    };

    return impl_->inner.decode(payload, inner_sink);
}

void QwenXmlDecoder::finish_stream(const StreamDecoderSink& sink)
{
    impl_->extractor.finish([&](const std::string& tail) {
        if (sink.on_text && !tail.empty()) sink.on_text(tail);
    });
}

void QwenXmlDecoder::reset()
{
    impl_->inner.reset();
    impl_->extractor.reset();
}

} // namespace locus
