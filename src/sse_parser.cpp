#include "sse_parser.h"

namespace locus {

SseParser::SseParser(DataCallback cb)
    : cb_(std::move(cb))
{
}

void SseParser::feed(const std::string& chunk)
{
    buffer_ += chunk;

    // Process all complete lines (terminated by \n or \r\n).
    std::string::size_type start = 0;
    while (true) {
        auto pos = buffer_.find('\n', start);
        if (pos == std::string::npos)
            break;

        std::string line = buffer_.substr(start, pos - start);

        // Strip trailing \r if present (handles \r\n).
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        process_line(line);
        start = pos + 1;
    }

    // Keep the remaining partial line in the buffer.
    if (start > 0)
        buffer_.erase(0, start);
}

void SseParser::finish()
{
    if (!buffer_.empty()) {
        // Strip trailing \r
        if (!buffer_.empty() && buffer_.back() == '\r')
            buffer_.pop_back();
        if (!buffer_.empty())
            process_line(buffer_);
        buffer_.clear();
    }
}

void SseParser::process_line(const std::string& line)
{
    // SSE spec: lines starting with "data:" carry the payload.
    // Empty lines are event separators — we ignore them since
    // OpenAI-compatible APIs send one data: line per event.
    // Lines starting with ":" are comments (keep-alive).
    // Other fields (event:, id:, retry:) are ignored.

    if (line.empty() || line[0] == ':')
        return;

    if (line.size() >= 5 && line.compare(0, 5, "data:") == 0) {
        std::string payload = line.substr(5);

        // Trim leading space (SSE spec: "data: value" → "value").
        if (!payload.empty() && payload[0] == ' ')
            payload.erase(0, 1);

        if (cb_)
            cb_(payload);
    }
    // All other fields (event:, id:, retry:) silently ignored.
}

} // namespace locus
