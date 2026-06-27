#pragma once

#include "text_extractor.h"

namespace locus {

// Extracts plain text and <h1>-<h6> headings from HTML.
//
// S6.1 -- the parser is gumbo (Apache-2.0, pure C HTML5). It walks the real DOM
// instead of the old regex stripper, so it handles malformed markup gracefully
// and drops chrome subtrees (script / style / nav / footer / header / aside)
// rather than only the obvious <script>/<style> blocks. The result is close to
// a browser "reader mode" view -- the right shape for both fetched web pages
// (S6.1) and ZIM articles (S6.2).
class HtmlExtractor : public ITextExtractor {
public:
    ExtractionResult extract(const std::filesystem::path& abs_path) override;

    // Strip an in-memory HTML string (no file I/O). The ZIM reader (S6.2) and
    // the web-fetch path (S6.1) both pull HTML that never touched disk, so they
    // need the same DOM walk + heading extraction the file path uses. extract()
    // routes through this after reading the file, so the two paths stay
    // identical.
    static ExtractionResult extract_from_string(const std::string& html);
};

} // namespace locus
