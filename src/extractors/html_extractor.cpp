#include "html_extractor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace locus {

namespace {

// Decode a tiny set of named entities + numeric &#NNN;/&#xNN; references.
// We don't ship a full HTML entity table — this covers the common cases
// (text extraction, not rendering).
std::string decode_entities(const std::string& in)
{
    std::string out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size(); ) {
        if (in[i] != '&') { out += in[i++]; continue; }

        // Find semicolon within a reasonable window
        size_t semi = in.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) {
            out += in[i++];
            continue;
        }

        std::string entity = in.substr(i + 1, semi - i - 1);
        bool handled = true;

        if (entity == "amp")         out += '&';
        else if (entity == "lt")     out += '<';
        else if (entity == "gt")     out += '>';
        else if (entity == "quot")   out += '"';
        else if (entity == "apos")   out += '\'';
        else if (entity == "nbsp")   out += ' ';
        else if (entity == "copy")   out += "(c)";
        else if (entity == "reg")    out += "(R)";
        else if (entity == "mdash")  out += "--";
        else if (entity == "ndash")  out += '-';
        else if (entity == "hellip") out += "...";
        else if (!entity.empty() && entity[0] == '#') {
            try {
                int cp = (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X'))
                    ? std::stoi(entity.substr(2), nullptr, 16)
                    : std::stoi(entity.substr(1));
                // Basic UTF-8 encoding
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            } catch (...) { handled = false; }
        } else {
            handled = false;
        }

        if (handled) {
            i = semi + 1;
        } else {
            out += in[i++];
        }
    }

    return out;
}

// Remove <script>...</script> and <style>...</style> blocks entirely.
std::string strip_noise_blocks(std::string s)
{
    static const std::regex script_re(R"(<script\b[^>]*>[\s\S]*?</script\s*>)",
                                      std::regex::icase);
    static const std::regex style_re(R"(<style\b[^>]*>[\s\S]*?</style\s*>)",
                                     std::regex::icase);
    s = std::regex_replace(s, script_re, " ");
    s = std::regex_replace(s, style_re,  " ");
    return s;
}

// Collapse runs of whitespace, preserving paragraph breaks.
std::string collapse_whitespace(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    int consecutive_newlines = 0;
    bool last_was_space = false;

    for (char c : s) {
        if (c == '\n') {
            if (consecutive_newlines < 2) out += '\n';
            ++consecutive_newlines;
            last_was_space = true;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            if (!last_was_space) out += ' ';
            last_was_space = true;
        } else {
            out += c;
            last_was_space = false;
            consecutive_newlines = 0;
        }
    }
    return out;
}

} // namespace

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

    // Extract <h1>-<h6> headings from raw HTML first (preserves document order
    // and line numbers before stripping).
    static const std::regex heading_re(R"(<h([1-6])[^>]*>([\s\S]*?)</h\1\s*>)",
                                       std::regex::icase);
    static const std::regex tag_re("<[^>]+>");

    auto begin = std::sregex_iterator(content.begin(), content.end(), heading_re);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        size_t pos = static_cast<size_t>(m.position());
        int line_num = 1 + static_cast<int>(
            std::count(content.begin(), content.begin() + pos, '\n'));

        int level = std::stoi(m[1].str());
        std::string text = decode_entities(
            std::regex_replace(m[2].str(), tag_re, ""));

        // Trim
        auto l = text.find_first_not_of(" \t\r\n");
        auto r = text.find_last_not_of(" \t\r\n");
        if (l == std::string::npos) continue;
        text = text.substr(l, r - l + 1);

        if (!text.empty()) {
            result.headings.push_back({ level, std::move(text), line_num });
        }
    }

    // Strip script/style blocks, then all tags, decode entities, collapse ws.
    std::string stripped = strip_noise_blocks(content);
    stripped = std::regex_replace(stripped, tag_re, " ");
    stripped = decode_entities(stripped);
    result.text = collapse_whitespace(stripped);

    return result;
}

} // namespace locus
