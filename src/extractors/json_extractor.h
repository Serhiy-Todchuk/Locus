#pragma once

#include "text_extractor.h"

namespace locus {

// Extracts full text + top-level keys (as level-1 headings) from JSON files.
// Not a real parser -- a forgiving line scanner that records the first quoted
// key seen at brace-depth 1. Enough to drive `get_file_outline` and
// `search_symbols` on configs without dragging a JSON parser into the indexer.
// True structural search lives on `mode=ast` via the Tree-sitter grammar.
class JsonExtractor : public ITextExtractor {
public:
    ExtractionResult extract(const std::filesystem::path& abs_path) override;
};

} // namespace locus
