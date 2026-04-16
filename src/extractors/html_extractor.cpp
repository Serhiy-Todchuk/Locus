#include "html_extractor.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <regex>

namespace locus {

ExtractionResult HtmlExtractor::extract(const std::filesystem::path& abs_path)
{
    std::ifstream f(abs_path, std::ios::binary);
    if (!f.is_open()) {
        spdlog::warn("HtmlExtractor: cannot open {}", abs_path.string());
        return { .is_binary = true };
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    ExtractionResult result;
    result.text = content;

    // Extract <h1>-<h6> headings
    static const std::regex heading_re(R"(<h([1-6])[^>]*>(.*?)</h\1>)",
                                       std::regex::icase);
    static const std::regex tag_re("<[^>]+>");

    size_t search_start = 0;
    std::smatch match;
    while (std::regex_search(content.cbegin() + search_start, content.cend(),
                             match, heading_re))
    {
        auto match_pos = static_cast<size_t>(match.position()) + search_start;
        int line_num = 1 + static_cast<int>(
            std::count(content.begin(), content.begin() + match_pos, '\n'));

        int level = std::stoi(match[1].str());
        std::string text = std::regex_replace(match[2].str(), tag_re, "");

        if (!text.empty()) {
            result.headings.push_back({ level, std::move(text), line_num });
        }

        search_start = match_pos + match.length();
    }

    return result;
}

} // namespace locus
