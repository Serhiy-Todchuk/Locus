#include "yaml_extractor.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace locus {

namespace {

// "Top-level key" heuristic: column-0 (no leading whitespace), starts with a
// printable non-`-` char, contains a `:` followed by either EOL or whitespace.
// Anchors (`&foo`), aliases (`*foo`), and multi-document `---` markers are
// ignored. This is a deliberately shallow scan -- YAML's full grammar lives in
// the Tree-sitter parser for `mode=ast`.
bool is_top_level_key_line(const std::string& line, std::string& key_out)
{
    if (line.empty()) return false;
    if (line[0] == ' ' || line[0] == '\t') return false;
    if (line[0] == '#' || line[0] == '-' || line[0] == '!' || line[0] == '%') return false;

    // Find `:` outside quotes
    bool in_single = false, in_double = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\'' && !in_double) { in_single = !in_single; continue; }
        if (c == '"'  && !in_single) { in_double = !in_double; continue; }
        if (in_single || in_double) continue;
        if (c == ':') {
            // Followed by EOL or whitespace?
            if (i + 1 == line.size() ||
                line[i + 1] == ' ' || line[i + 1] == '\t' ||
                line[i + 1] == '\r') {
                std::string key = line.substr(0, i);
                // Strip optional surrounding quotes
                if (key.size() >= 2 &&
                    ((key.front() == '"' && key.back() == '"') ||
                     (key.front() == '\'' && key.back() == '\''))) {
                    key = key.substr(1, key.size() - 2);
                }
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                if (key.empty()) return false;
                key_out = std::move(key);
                return true;
            }
            return false;
        }
    }
    return false;
}

} // namespace

ExtractionResult YamlExtractor::extract(const std::filesystem::path& abs_path)
{
    std::ifstream f(abs_path, std::ios::binary);
    if (!f.is_open()) {
        spdlog::warn("YamlExtractor: cannot open {}", abs_path.string());
        return { .is_binary = true };
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    ExtractionResult result;
    result.text = content;

    std::istringstream stream(result.text);
    std::string line;
    int line_num = 0;
    while (std::getline(stream, line)) {
        ++line_num;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string key;
        if (is_top_level_key_line(line, key)) {
            result.headings.push_back({ 1, std::move(key), line_num });
        }
    }

    return result;
}

} // namespace locus
