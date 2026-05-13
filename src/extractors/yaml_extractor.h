#pragma once

#include "text_extractor.h"

namespace locus {

// Extracts full text + top-level keys (column-0 `key:` lines) as level-1
// headings. Same role as JsonExtractor: keep `get_file_outline` and
// `search_symbols` useful for configs without parsing the whole document.
// True structural search lives on `mode=ast` via the Tree-sitter grammar.
class YamlExtractor : public ITextExtractor {
public:
    ExtractionResult extract(const std::filesystem::path& abs_path) override;
};

} // namespace locus
