#pragma once

#include "extractors/text_extractor.h"

#include <string>
#include <vector>

namespace locus {

struct Chunk {
    int start_line = 0;   // 1-based inclusive
    int end_line = 0;     // 1-based inclusive
    std::string content;
};

// Symbol boundary info (simplified from SymbolResult for chunking only).
struct SymbolSpan {
    int line_start = 0;
    int line_end = 0;
};

// Split content at function/class boundaries.
// Symbols > max_lines are sub-split with overlap.
std::vector<Chunk> chunk_code(const std::string& content,
                              const std::vector<SymbolSpan>& symbols,
                              int max_lines, int overlap);

// Split content at heading boundaries (H2/H3).
// Sections > max_lines get sliding-window sub-split.
std::vector<Chunk> chunk_document(const std::string& content,
                                  const std::vector<ExtractedHeading>& headings,
                                  int max_lines, int overlap);

// Generic fallback: sliding window with overlap.
std::vector<Chunk> chunk_sliding_window(const std::string& content,
                                        int max_lines, int overlap);

} // namespace locus
