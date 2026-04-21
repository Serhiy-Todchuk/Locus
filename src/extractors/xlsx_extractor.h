#pragma once

#include "text_extractor.h"

namespace locus {

// Extracts plain text from OOXML .xlsx files.
// Unzips the archive via miniz, reads xl/sharedStrings.xml + each
// xl/worksheets/sheet*.xml via pugixml.  Emits one heading per sheet.
class XlsxExtractor : public ITextExtractor {
public:
    ExtractionResult extract(const std::filesystem::path& abs_path) override;
};

} // namespace locus
