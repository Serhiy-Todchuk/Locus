#include "llm/stream_decoders/auto_decoder.h"

#include "llm/stream_decoders/openai_decoder.h"
#include "llm/xml_tool_call_extractor.h"

namespace locus {

struct AutoToolFormatDecoder::Impl {
    OpenAiDecoder        inner;
    XmlToolCallExtractor extractor;

    Impl() : extractor({XmlMarker::Qwen, XmlMarker::Claude}) {}
};

AutoToolFormatDecoder::AutoToolFormatDecoder() : impl_(std::make_unique<Impl>()) {}
AutoToolFormatDecoder::~AutoToolFormatDecoder() = default;

bool AutoToolFormatDecoder::decode(const std::string& payload,
                                   const StreamDecoderSink& sink)
{
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

void AutoToolFormatDecoder::finish_stream(const StreamDecoderSink& sink)
{
    impl_->extractor.finish([&](const std::string& tail) {
        if (sink.on_text && !tail.empty()) sink.on_text(tail);
    });
}

void AutoToolFormatDecoder::reset()
{
    impl_->inner.reset();
    impl_->extractor.reset();
}

} // namespace locus
