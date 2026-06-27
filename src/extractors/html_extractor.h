#pragma once

#include "text_extractor.h"

namespace locus {

// Extracts plain text and <h1>-<h6> headings from HTML files.
class HtmlExtractor : public ITextExtractor {
public:
    ExtractionResult extract(const std::filesystem::path& abs_path) override;

    // S6.2 -- strip an in-memory HTML string (no file I/O). The ZIM reader
    // pulls article HTML straight out of the archive and never writes it to
    // disk, so it needs the same script/style strip + tag removal + entity
    // decode + heading extraction that the file path uses. extract() routes
    // through this after reading the file, so the two paths stay identical.
    static ExtractionResult extract_from_string(const std::string& html);
};

} // namespace locus
