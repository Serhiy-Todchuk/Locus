#pragma once

#include "text_extractor.h"

namespace locus {

// Extracts plain text + headings from OOXML .docx files.
// Unzips the archive via miniz, parses word/document.xml via pugixml.
// Headings are paragraphs with pStyle="Heading1".."Heading6".
class DocxExtractor : public ITextExtractor {
public:
    ExtractionResult extract(const std::filesystem::path& abs_path) override;
};

} // namespace locus
