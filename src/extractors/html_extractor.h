#pragma once

#include "text_extractor.h"

namespace locus {

// Extracts plain text and <h1>-<h6> headings from HTML files.
class HtmlExtractor : public ITextExtractor {
public:
    ExtractionResult extract(const std::filesystem::path& abs_path) override;
};

} // namespace locus
