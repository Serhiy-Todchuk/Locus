#include "tools/truncation_detector.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace locus::tools {

namespace {

// Canonical phrase list. Each entry is checked against the lower-cased tail
// of `content`. The "(eof_only)" flag distinguishes phrases that legitimately
// appear mid-file (TODO comments anywhere in source) from elision markers
// that only matter when they sit at the very end.
struct Phrase {
    const char* text;     // already lower-case
    bool        eof_only; // when true, only fire when phrase is in the very
                          // last 64 chars (post-trim). "// TODO: implement"
                          // is real anywhere -- only a problem at EOF.
};

constexpr std::array<Phrase, 12> k_phrases = {{
    {"// rest of the code",          false},
    {"// rest of the implementation", false},
    {"// ... existing code ...",     false},
    {"// (continued)",               false},
    {"// todo: implement",           true},
    {"# rest of the code",           false},
    {"# rest of the implementation", false},
    {"# (continued)",                false},
    {"# todo: implement",            true},
    {"/* ... rest ... */",           false},
    {"/* rest of the code */",       false},
    {"<!-- rest of the code -->",    false},
}};

// Return the trailing slice of `content` (up to last `cap` chars), folded to
// lower-case. Trailing whitespace is trimmed first so a phrase followed by a
// closing brace / newline / spaces still matches.
std::string lower_tail(std::string_view content, size_t cap)
{
    // Strip trailing whitespace, then take the last `cap` chars of what's left.
    size_t end = content.size();
    while (end > 0) {
        unsigned char c = static_cast<unsigned char>(content[end - 1]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            --end;
            continue;
        }
        break;
    }
    size_t start = end > cap ? end - cap : 0;
    std::string out;
    out.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(content[i]))));
    }
    return out;
}

} // namespace

std::optional<std::string> detect_truncation_phrase(std::string_view content)
{
    if (content.empty()) return std::nullopt;

    // 200-char tail balances precision vs. catching markers that come a line
    // or two before a trailing closing brace.
    std::string tail = lower_tail(content, 200);
    if (tail.empty()) return std::nullopt;

    // 64-char "very end" window for the eof_only phrases.
    std::string tail_end = tail.size() > 64
        ? tail.substr(tail.size() - 64)
        : tail;

    for (const auto& p : k_phrases) {
        const std::string& hay = p.eof_only ? tail_end : tail;
        if (hay.find(p.text) != std::string::npos) {
            return std::string(p.text);
        }
    }
    return std::nullopt;
}

} // namespace locus::tools
