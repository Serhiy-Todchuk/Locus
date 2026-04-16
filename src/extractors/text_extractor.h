#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace locus {

// A heading extracted from document content (markdown, HTML, PDF, etc.)
struct ExtractedHeading {
    int level = 0;          // 1-6
    std::string text;
    int line_number = 0;    // 1-based, approximate for binary formats
};

// Result of extracting text from a file.  The indexer uses this to populate
// FTS5 and headings tables.  If is_binary is true the file is logged and
// skipped — text/headings are ignored.
struct ExtractionResult {
    std::string text;                           // plain text for FTS5
    std::vector<ExtractedHeading> headings;
    bool is_binary = false;                     // couldn't extract (encrypted, etc.)
};

// Pure interface.  Implementations live in src/extractors/ — one per format.
class ITextExtractor {
public:
    virtual ~ITextExtractor() = default;

    // Extract readable text + headings from the file at abs_path.
    // Implementations should NOT throw on recoverable failures — return
    // ExtractionResult{.is_binary = true} instead.
    virtual ExtractionResult extract(const std::filesystem::path& abs_path) = 0;
};

} // namespace locus
