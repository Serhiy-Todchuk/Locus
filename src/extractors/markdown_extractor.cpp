#include "markdown_extractor.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace locus {

ExtractionResult MarkdownExtractor::extract(const std::filesystem::path& abs_path)
{
    std::ifstream f(abs_path, std::ios::binary);
    if (!f.is_open()) {
        spdlog::warn("MarkdownExtractor: cannot open {}", abs_path.string());
        return { .is_binary = true };
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    ExtractionResult result;
    result.text = content;

    // Parse # headings line by line
    std::istringstream stream(content);
    std::string line;
    int line_num = 0;
    while (std::getline(stream, line)) {
        ++line_num;
        if (line.empty() || line[0] != '#') continue;

        int level = 0;
        while (level < static_cast<int>(line.size()) && line[level] == '#') ++level;
        if (level > 6 || level >= static_cast<int>(line.size())) continue;
        if (line[level] != ' ') continue;   // require space after #

        std::string text = line.substr(level + 1);
        // Trim trailing whitespace and #
        while (!text.empty() && (text.back() == ' ' || text.back() == '#' || text.back() == '\r'))
            text.pop_back();

        if (text.empty()) continue;

        result.headings.push_back({ level, std::move(text), line_num });
    }

    return result;
}

} // namespace locus
