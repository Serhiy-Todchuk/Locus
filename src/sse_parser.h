#pragma once

#include <functional>
#include <string>

namespace locus {

// Incremental SSE (Server-Sent Events) line parser.
// Feed it raw bytes from an HTTP response body; it calls back with
// complete `data:` payloads (one per event). Handles partial lines
// that arrive across chunk boundaries.
class SseParser {
public:
    // Called with the payload of each `data:` line (trimmed).
    // Returns false to abort parsing.
    using DataCallback = std::function<bool(const std::string& data)>;

    explicit SseParser(DataCallback cb);

    // Feed a chunk of raw bytes. May invoke the callback zero or more times.
    void feed(const std::string& chunk);

    // Call after the last chunk to flush any buffered partial line.
    void finish();

private:
    void process_line(const std::string& line);

    DataCallback cb_;
    std::string  buffer_;
};

} // namespace locus
