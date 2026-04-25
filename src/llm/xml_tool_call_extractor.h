#pragma once

#include "llm/stream_decoder.h"   // StreamDecoderSink::ToolCallDelta

#include <functional>
#include <string>
#include <vector>

namespace locus {

// Which XML tool-call dialect a marker belongs to. The extractor watches
// for the opening token; the closing token is implied by the dialect.
enum class XmlMarker {
    Qwen,    // <tool_call> {"name":"...","arguments":{...}} </tool_call>
    Claude   // <function_calls><invoke name="..."><parameter name="...">...</parameter></invoke></function_calls>
};

// Streaming filter that scans a text channel for embedded tool calls and
// extracts them. Text outside the markers is forwarded verbatim; text
// inside is buffered until the closing tag, then parsed and emitted as
// one or more ToolCallDelta events. Designed to be fed chunk-by-chunk
// from inside an IStreamDecoder.
//
// Watches the markers in the order given. As soon as one fires, the
// extractor "locks" to that dialect for the remainder of the stream --
// avoids false positives on unrelated XML appearing later.
class XmlToolCallExtractor {
public:
    using OnText      = std::function<void(const std::string& text)>;
    using OnToolCall  = std::function<void(const StreamDecoderSink::ToolCallDelta& d)>;

    explicit XmlToolCallExtractor(std::vector<XmlMarker> watch);

    // Feed a fresh chunk of text. Calls `on_text` for any portion that
    // sits outside a tool-call marker, and `on_tool_call` once for each
    // fully closed tool call seen in this or earlier chunks.
    void feed(const std::string& chunk, const OnText& on_text,
              const OnToolCall& on_tool_call);

    // Stream is closing -- flush whatever is left. If we are still inside
    // a tool call (truncated stream), the partial buffer is forwarded as
    // text so the user can at least see what came through. Outside a
    // marker, any pending partial-tag bytes are also flushed as text.
    void finish(const OnText& on_text);

    // Reset all state for a new stream. Call between streams that share
    // the same extractor instance.
    void reset();

    // Whether the extractor has emitted at least one tool call. Useful
    // for the auto-detect decoder to lock the format.
    bool has_emitted() const { return next_index_ > 0; }

private:
    enum class State {
        Outside,
        InQwen,
        InClaude
    };

    // Returns true and advances state if a complete opening token was
    // found in `buf_`. Outputs the consumed prefix as text via on_text.
    bool try_open(const OnText& on_text);

    // While inside a marker, returns true (and resets state) once the
    // matching closing token is present. Emits the parsed tool call(s).
    bool try_close(const OnToolCall& on_tool_call);

    // After processing what we can, decide how much of buf_ is safe to
    // emit (i.e. cannot be a partial-marker prefix) and emit it.
    void emit_safe_outside_text(const OnText& on_text);

    static void parse_qwen_body(const std::string& body, int& next_index,
                                const OnToolCall& on_tool_call);
    static void parse_claude_body(const std::string& body, int& next_index,
                                  const OnToolCall& on_tool_call);

    std::vector<XmlMarker> watch_;
    State state_   = State::Outside;
    std::string buf_;        // pending text outside, body content inside
    int next_index_ = 0;     // tool-call index handed to ToolCallDelta
};

} // namespace locus
