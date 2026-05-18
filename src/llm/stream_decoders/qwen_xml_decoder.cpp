#include "llm/stream_decoders/qwen_xml_decoder.h"

#include "llm/stream_decoders/openai_decoder.h"
#include "llm/xml_tool_call_extractor.h"

namespace locus {

// One extractor per channel (content + reasoning); shared counter for
// `index` / synthesized id uniqueness. See auto_decoder.cpp for rationale.
struct QwenXmlDecoder::Impl {
    OpenAiDecoder        inner;
    XmlToolCallExtractor content_ext;
    XmlToolCallExtractor reason_ext;
    int                  shared_index = 0;

    Impl()
        : content_ext({XmlMarker::Qwen})
        , reason_ext({XmlMarker::Qwen})
    {
    }
};

QwenXmlDecoder::QwenXmlDecoder() : impl_(std::make_unique<Impl>()) {}
QwenXmlDecoder::~QwenXmlDecoder() = default;

bool QwenXmlDecoder::decode(const std::string& payload,
                            const StreamDecoderSink& sink)
{
    StreamDecoderSink inner_sink;
    inner_sink.on_tool_call_delta = sink.on_tool_call_delta;
    inner_sink.on_usage           = sink.on_usage;
    inner_sink.on_finish_reason   = sink.on_finish_reason;
    inner_sink.on_stream_error    = sink.on_stream_error;

    inner_sink.on_text = [&](const std::string& token) {
        impl_->content_ext.set_next_index(impl_->shared_index);
        impl_->content_ext.feed(
            token,
            [&](const std::string& clean) {
                if (sink.on_text && !clean.empty()) sink.on_text(clean);
            },
            [&](const StreamDecoderSink::ToolCallDelta& d) {
                if (sink.on_tool_call_delta) sink.on_tool_call_delta(d);
            }
        );
        impl_->shared_index = impl_->content_ext.next_index();
    };

    inner_sink.on_reasoning = [&](const std::string& token) {
        impl_->reason_ext.set_next_index(impl_->shared_index);
        impl_->reason_ext.feed(
            token,
            [&](const std::string& clean) {
                if (sink.on_reasoning && !clean.empty()) sink.on_reasoning(clean);
            },
            [&](const StreamDecoderSink::ToolCallDelta& d) {
                if (sink.on_tool_call_delta) sink.on_tool_call_delta(d);
            }
        );
        impl_->shared_index = impl_->reason_ext.next_index();
    };

    return impl_->inner.decode(payload, inner_sink);
}

void QwenXmlDecoder::finish_stream(const StreamDecoderSink& sink)
{
    impl_->content_ext.finish([&](const std::string& tail) {
        if (sink.on_text && !tail.empty()) sink.on_text(tail);
    });
    impl_->reason_ext.finish([&](const std::string& tail) {
        if (sink.on_reasoning && !tail.empty()) sink.on_reasoning(tail);
    });
}

void QwenXmlDecoder::reset()
{
    impl_->inner.reset();
    impl_->content_ext.reset();
    impl_->reason_ext.reset();
    impl_->shared_index = 0;
}

} // namespace locus
