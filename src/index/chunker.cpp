#include "chunker.h"

#include <algorithm>
#include <sstream>

namespace locus {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Split content into lines, preserving original line numbers (1-based).
static std::vector<std::string> split_lines(const std::string& content)
{
    std::vector<std::string> lines;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

// Build a Chunk from a range of lines [start, end) (0-based indices).
static Chunk make_chunk(const std::vector<std::string>& lines,
                        int begin_idx, int end_idx)
{
    Chunk c;
    c.start_line = begin_idx + 1;  // 1-based
    c.end_line = end_idx;          // 1-based inclusive

    std::string text;
    for (int i = begin_idx; i < end_idx; ++i) {
        if (i > begin_idx) text.push_back('\n');
        text.append(lines[i]);
    }
    c.content = std::move(text);
    return c;
}

// Sub-split a line range with sliding window + overlap.
static void sliding_split(const std::vector<std::string>& lines,
                          int begin_idx, int end_idx,
                          int max_lines, int overlap,
                          std::vector<Chunk>& out)
{
    int len = end_idx - begin_idx;
    if (len <= max_lines) {
        out.push_back(make_chunk(lines, begin_idx, end_idx));
        return;
    }

    int step = max_lines - overlap;
    if (step < 1) step = 1;

    for (int pos = begin_idx; pos < end_idx; pos += step) {
        int chunk_end = std::min(pos + max_lines, end_idx);
        out.push_back(make_chunk(lines, pos, chunk_end));
        if (chunk_end == end_idx) break;
    }
}

// ---------------------------------------------------------------------------
// chunk_code — split at symbol (function/class) boundaries
// ---------------------------------------------------------------------------

std::vector<Chunk> chunk_code(const std::string& content,
                              const std::vector<SymbolSpan>& symbols,
                              int max_lines, int overlap)
{
    auto lines = split_lines(content);
    int total = static_cast<int>(lines.size());

    if (symbols.empty()) {
        return chunk_sliding_window(content, max_lines, overlap);
    }

    // Sort symbols by start line
    auto sorted = symbols;
    std::sort(sorted.begin(), sorted.end(),
              [](const SymbolSpan& a, const SymbolSpan& b) {
                  return a.line_start < b.line_start;
              });

    std::vector<Chunk> chunks;
    int cursor = 0;  // 0-based line index

    for (const auto& sym : sorted) {
        int sym_begin = sym.line_start - 1;  // convert to 0-based
        int sym_end = std::min(sym.line_end, total);

        if (sym_begin < 0) sym_begin = 0;
        if (sym_begin < cursor) sym_begin = cursor;  // overlapping symbols
        if (sym_begin >= total) break;

        // Gap before this symbol
        if (cursor < sym_begin) {
            sliding_split(lines, cursor, sym_begin, max_lines, overlap, chunks);
        }

        // The symbol itself
        sliding_split(lines, sym_begin, sym_end, max_lines, overlap, chunks);
        cursor = sym_end;
    }

    // Trailing content after last symbol
    if (cursor < total) {
        sliding_split(lines, cursor, total, max_lines, overlap, chunks);
    }

    return chunks;
}

// ---------------------------------------------------------------------------
// chunk_document — split at heading boundaries
// ---------------------------------------------------------------------------

std::vector<Chunk> chunk_document(const std::string& content,
                                  const std::vector<ExtractedHeading>& headings,
                                  int max_lines, int overlap)
{
    auto lines = split_lines(content);
    int total = static_cast<int>(lines.size());

    if (headings.empty()) {
        return chunk_sliding_window(content, max_lines, overlap);
    }

    // Collect split points from H2/H3 headings (1-based line numbers)
    std::vector<int> splits;
    for (const auto& h : headings) {
        if (h.level <= 3 && h.line_number >= 1 && h.line_number <= total) {
            splits.push_back(h.line_number - 1);  // 0-based
        }
    }
    std::sort(splits.begin(), splits.end());
    splits.erase(std::unique(splits.begin(), splits.end()), splits.end());

    if (splits.empty()) {
        return chunk_sliding_window(content, max_lines, overlap);
    }

    std::vector<Chunk> chunks;
    int cursor = 0;

    for (int split : splits) {
        if (split > cursor) {
            sliding_split(lines, cursor, split, max_lines, overlap, chunks);
        }
        cursor = split;
    }

    // Final section
    if (cursor < total) {
        sliding_split(lines, cursor, total, max_lines, overlap, chunks);
    }

    return chunks;
}

// ---------------------------------------------------------------------------
// chunk_sliding_window — generic fallback
// ---------------------------------------------------------------------------

std::vector<Chunk> chunk_sliding_window(const std::string& content,
                                        int max_lines, int overlap)
{
    auto lines = split_lines(content);
    int total = static_cast<int>(lines.size());

    std::vector<Chunk> chunks;
    sliding_split(lines, 0, total, max_lines, overlap, chunks);
    return chunks;
}

} // namespace locus
