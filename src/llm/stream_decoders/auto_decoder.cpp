#include "llm/stream_decoders/auto_decoder.h"

#include "llm/stream_decoders/openai_decoder.h"
#include "llm/xml_tool_call_extractor.h"

namespace locus {

// Two extractor instances -- one per channel (content + reasoning) -- so a
// model that emits its tool call inside <think>/reasoning_content (LM Studio
// reasoning models, Qwen3 with thinking on, etc.) still gets the call
// dispatched instead of leaving it as dead text in the chat reasoning bubble.
// A single shared counter keeps the synthesized `call_xml_<n>` ids and the
// ToolCallDelta `index` slots globally unique across channels -- otherwise
// LMStudioClient's per-index coalescing would merge unrelated calls.
struct AutoToolFormatDecoder::Impl {
    OpenAiDecoder        inner;
    XmlToolCallExtractor content_ext;
    XmlToolCallExtractor reason_ext;
    int                  shared_index = 0;

    Impl()
        : content_ext({XmlMarker::Qwen, XmlMarker::Claude})
        , reason_ext({XmlMarker::Qwen, XmlMarker::Claude})
    {
    }
};

AutoToolFormatDecoder::AutoToolFormatDecoder() : impl_(std::make_unique<Impl>()) {}
AutoToolFormatDecoder::~AutoToolFormatDecoder() = default;

bool AutoToolFormatDecoder::decode(const std::string& payload,
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

void AutoToolFormatDecoder::finish_stream(const StreamDecoderSink& sink)
{
    impl_->content_ext.finish([&](const std::string& tail) {
        if (sink.on_text && !tail.empty()) sink.on_text(tail);
    });
    impl_->reason_ext.finish([&](const std::string& tail) {
        if (sink.on_reasoning && !tail.empty()) sink.on_reasoning(tail);
    });
}

void AutoToolFormatDecoder::reset()
{
    impl_->inner.reset();
    impl_->content_ext.reset();
    impl_->reason_ext.reset();
    impl_->shared_index = 0;
}

} // namespace locus
