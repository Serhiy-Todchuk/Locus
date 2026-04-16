#pragma once

#include "text_extractor.h"

namespace locus {

// Extracts plain text and # headings from Markdown files.
class MarkdownExtractor : public ITextExtractor {
public:
    ExtractionResult extract(const std::filesystem::path& abs_path) override;
};

} // namespace locus
