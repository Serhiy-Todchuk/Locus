#include "html_extractor.h"

#include <spdlog/spdlog.h>

#include <gumbo.h>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace locus {

namespace {

// Tags whose subtrees are dropped entirely from the extracted text. script /
// style are never readable; nav / footer / header / aside are chrome that
// pollutes the FTS index with menus, cookie banners, and copyright boilerplate
// (S6.1 extraction rule + architecture/web-retrieval.md). The result is close
// to a browser "reader mode" view.
bool is_skipped_subtree(GumboTag tag)
{
    switch (tag) {
        case GUMBO_TAG_SCRIPT:
        case GUMBO_TAG_STYLE:
        case GUMBO_TAG_NOSCRIPT:
        case GUMBO_TAG_NAV:
        case GUMBO_TAG_FOOTER:
        case GUMBO_TAG_HEADER:
        case GUMBO_TAG_ASIDE:
        case GUMBO_TAG_TEMPLATE:
            return true;
        default:
            return false;
    }
}

// Block-level tags: append a newline after their content so paragraph and list
// boundaries survive into the plain text (readable chunking for the indexer).
bool is_block(GumboTag tag)
{
    switch (tag) {
        case GUMBO_TAG_P:
        case GUMBO_TAG_DIV:
        case GUMBO_TAG_BR:
        case GUMBO_TAG_LI:
        case GUMBO_TAG_UL:
        case GUMBO_TAG_OL:
        case GUMBO_TAG_TR:
        case GUMBO_TAG_TABLE:
        case GUMBO_TAG_SECTION:
        case GUMBO_TAG_ARTICLE:
        case GUMBO_TAG_BLOCKQUOTE:
        case GUMBO_TAG_PRE:
        case GUMBO_TAG_H1:
        case GUMBO_TAG_H2:
        case GUMBO_TAG_H3:
        case GUMBO_TAG_H4:
        case GUMBO_TAG_H5:
        case GUMBO_TAG_H6:
            return true;
        default:
            return false;
    }
}

// Heading level (1-6) for an h1..h6 tag, or 0 otherwise.
int heading_level(GumboTag tag)
{
    switch (tag) {
        case GUMBO_TAG_H1: return 1;
        case GUMBO_TAG_H2: return 2;
        case GUMBO_TAG_H3: return 3;
        case GUMBO_TAG_H4: return 4;
        case GUMBO_TAG_H5: return 5;
        case GUMBO_TAG_H6: return 6;
        default:           return 0;
    }
}

std::string trim(const std::string& s)
{
    auto l = s.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    auto r = s.find_last_not_of(" \t\r\n");
    return s.substr(l, r - l + 1);
}

// Concatenate all descendant text of `node` (used to build a heading's text;
// headings often wrap their text in <a> / <span>).
void gather_text(const GumboNode* node, std::string& out)
{
    if (!node) return;
    if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_CDATA) {
        out += node->v.text.text;
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT && node->type != GUMBO_NODE_TEMPLATE)
        return;
    if (is_skipped_subtree(node->v.element.tag)) return;
    const GumboVector* kids = &node->v.element.children;
    for (unsigned int i = 0; i < kids->length; ++i)
        gather_text(static_cast<const GumboNode*>(kids->data[i]), out);
}

// Collapse runs of whitespace, preserving up to two consecutive newlines so
// paragraph breaks remain. Same shape the old regex path produced so snippet
// rendering stays stable.
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

// Depth-first walk emitting visible text + heading records. Headings record the
// 1-based source line (gumbo's start_pos.line) before the text is collapsed.
void walk(const GumboNode* node, std::string& text,
          std::vector<ExtractedHeading>& headings)
{
    if (!node) return;

    if (node->type == GUMBO_NODE_TEXT) {
        text += node->v.text.text;
        return;
    }
    if (node->type == GUMBO_NODE_CDATA) {
        text += node->v.text.text;
        return;
    }
    if (node->type == GUMBO_NODE_WHITESPACE) {
        text += ' ';
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT && node->type != GUMBO_NODE_TEMPLATE)
        return;

    const GumboElement& el = node->v.element;
    if (is_skipped_subtree(el.tag)) return;

    int level = heading_level(el.tag);
    if (level > 0) {
        std::string htext;
        gather_text(node, htext);
        htext = trim(collapse_whitespace(htext));
        if (!htext.empty()) {
            int line = static_cast<int>(el.start_pos.line);
            headings.push_back({ level, htext, line > 0 ? line : 1 });
        }
    }

    const GumboVector* kids = &el.children;
    for (unsigned int i = 0; i < kids->length; ++i)
        walk(static_cast<const GumboNode*>(kids->data[i]), text, headings);

    if (is_block(el.tag)) text += '\n';
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
    return extract_from_string(content);
}

ExtractionResult HtmlExtractor::extract_from_string(const std::string& content)
{
    ExtractionResult result;
    if (content.empty()) return result;

    // gumbo_parse requires a NUL-terminated buffer; std::string::c_str() gives
    // exactly that and the buffer outlives the parse tree here.
    GumboOutput* output = gumbo_parse(content.c_str());
    if (!output) {
        spdlog::warn("HtmlExtractor: gumbo_parse returned null");
        return result;
    }

    std::string raw;
    raw.reserve(content.size() / 2);
    walk(output->root, raw, result.headings);
    gumbo_destroy_output(&kGumboDefaultOptions, output);

    result.text = collapse_whitespace(raw);
    return result;
}

} // namespace locus
