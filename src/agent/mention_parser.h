#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace locus {

// One `@path/to/file` reference inside a user message. Byte offsets are
// into the original UTF-8 string; the parser does not transform the text
// (callers can use `start`/`end` to highlight, replace, or strip in place).
//
// The S4.V `@`-mention feature is purely a typing-time helper plus a
// submit-time auto-attach signal -- the path text itself stays inside the
// user message so the LLM sees the mention verbatim.
struct Mention {
    size_t      start = 0;  // byte offset of the leading '@'
    size_t      end   = 0;  // one-past-end byte offset of the last path char
    std::string path;       // raw text after '@', without the '@'
};

// Scan `text` for `@<path>` tokens and return them in source order.
//
// Recognised:
//   - `@` must be preceded by start-of-text, whitespace, or one of a small
//     punctuation set (`(`, `[`, `,`, `;`, `:`). This is the cheap
//     heuristic that keeps `user@example.com` from being treated as an
//     attach intent.
//   - The path continues until whitespace, end-of-text, or one of
//     `)`, `]`, `,`, `;` (trailing punctuation users naturally type after
//     a path). Trailing dots are also dropped (e.g. "see @src/foo.cpp.")
//     so the sentence-ending period doesn't end up in the path.
//   - The path must contain at least one non-space character.
//
// The parser does NOT validate that the path exists -- that's the caller's
// job (it usually wants `IndexQuery::list_directory` for that).
std::vector<Mention> parse_mentions(std::string_view text);

} // namespace locus
