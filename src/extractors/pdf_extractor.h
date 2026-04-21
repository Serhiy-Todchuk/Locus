#pragma once

#include "text_extractor.h"

namespace locus {

// Extracts plain text from PDF files via PDFium.
// One "pseudo-heading" per page is emitted (line = page's first line),
// so readers can locate content by page.  Encrypted/unreadable PDFs are
// logged and returned with is_binary = true.
class PdfiumExtractor : public ITextExtractor {
public:
    PdfiumExtractor();
    ~PdfiumExtractor() override;

    ExtractionResult extract(const std::filesystem::path& abs_path) override;

    PdfiumExtractor(const PdfiumExtractor&) = delete;
    PdfiumExtractor& operator=(const PdfiumExtractor&) = delete;
};

} // namespace locus
