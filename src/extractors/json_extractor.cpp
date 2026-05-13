#include "json_extractor.h"

#include <spdlog/spdlog.h>

#include <fstream>

namespace locus {

namespace {

// Walk `content` once. Whenever we're at object-depth 1 and outside any
// string, record the first quoted key followed by `:` on each line.
// Comments are not in the JSON spec but `.json` files in the wild
// (jsonc, settings.json) often use `//` / `/* */`; we honour both.
std::vector<ExtractedHeading> scan_top_level_keys(std::string_view content)
{
    std::vector<ExtractedHeading> out;

    int  brace_depth = 0;   // {} stack
    int  array_depth = 0;   // [] stack (depth-1 inside a root array means index-shaped, no key)
    bool in_string   = false;
    bool escape_next = false;
    bool in_line_comment  = false;
    bool in_block_comment = false;

    int   line = 1;
    int   key_start_line = 0;
    std::string current_key;
    bool  collecting_key = false;
    bool  saw_key_this_line = false;
    bool  line_has_value_before_key = false; // suppress nested-only lines
    (void)line_has_value_before_key;

    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];

        if (c == '\n') {
            ++line;
            saw_key_this_line = false;
            if (in_line_comment) in_line_comment = false;
            continue;
        }

        if (in_line_comment) continue;
        if (in_block_comment) {
            if (c == '*' && i + 1 < content.size() && content[i + 1] == '/') {
                in_block_comment = false;
                ++i;
            }
            continue;
        }

        if (in_string) {
            if (escape_next) { escape_next = false; if (collecting_key) current_key.push_back(c); continue; }
            if (c == '\\')   { escape_next = true;  if (collecting_key) current_key.push_back(c); continue; }
            if (c == '"') {
                in_string = false;
                continue;
            }
            if (collecting_key) current_key.push_back(c);
            continue;
        }

        // Not in string
        if (c == '/' && i + 1 < content.size()) {
            if (content[i + 1] == '/') { in_line_comment  = true;  ++i; continue; }
            if (content[i + 1] == '*') { in_block_comment = true;  ++i; continue; }
        }
        if (c == '"') {
            in_string = true;
            if (brace_depth == 1 && array_depth == 0 && !saw_key_this_line) {
                collecting_key = true;
                current_key.clear();
                key_start_line = line;
            } else {
                collecting_key = false;
            }
            continue;
        }
        if (c == ':') {
            if (collecting_key && !current_key.empty()) {
                out.push_back({ 1, current_key, key_start_line });
                saw_key_this_line = true;
            }
            collecting_key = false;
            current_key.clear();
            continue;
        }
        if (c == ',') {
            saw_key_this_line = false;
            collecting_key = false;
            current_key.clear();
            continue;
        }
        if (c == '{') { ++brace_depth; collecting_key = false; continue; }
        if (c == '}') { --brace_depth; if (brace_depth < 0) brace_depth = 0; collecting_key = false; continue; }
        if (c == '[') { ++array_depth; collecting_key = false; continue; }
        if (c == ']') { --array_depth; if (array_depth < 0) array_depth = 0; collecting_key = false; continue; }
    }

    return out;
}

} // namespace

ExtractionResult JsonExtractor::extract(const std::filesystem::path& abs_path)
{
    std::ifstream f(abs_path, std::ios::binary);
    if (!f.is_open()) {
        spdlog::warn("JsonExtractor: cannot open {}", abs_path.string());
        return { .is_binary = true };
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    ExtractionResult result;
    result.text     = content;
    result.headings = scan_top_level_keys(result.text);
    return result;
}

} // namespace locus
