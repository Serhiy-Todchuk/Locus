#include "docx_extractor.h"
#include "zip_reader.h"

#include <spdlog/spdlog.h>

#include <pugixml.hpp>

#include <cstring>
#include <functional>
#include <string>

namespace locus {

namespace {

// DOCX uses the "w:" namespace; pugixml exposes qualified names verbatim.
// Walk any node, concatenate all <w:t> text runs, insert tabs/newlines for
// <w:tab>, <w:br>, <w:cr>.
void append_paragraph_text(const pugi::xml_node& p, std::string& out)
{
    std::function<void(const pugi::xml_node&)> visit = [&](const pugi::xml_node& node) {
        for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
            const char* name = c.name();
            if (std::strcmp(name, "w:t") == 0) {
                out += c.text().as_string();
            } else if (std::strcmp(name, "w:tab") == 0) {
                out += '\t';
            } else if (std::strcmp(name, "w:br") == 0 ||
                       std::strcmp(name, "w:cr") == 0) {
                out += '\n';
            } else {
                visit(c);
            }
        }
    };
    visit(p);
}

// Extract heading level from paragraph style ("Heading1" → 1, etc.).
// Returns 0 if not a heading.
int heading_level(const pugi::xml_node& p)
{
    pugi::xml_node pPr = p.child("w:pPr");
    if (!pPr) return 0;
    pugi::xml_node pStyle = pPr.child("w:pStyle");
    if (!pStyle) return 0;
    const char* val = pStyle.attribute("w:val").value();
    if (!val || !*val) return 0;

    // Common styles: "Heading1".."Heading9", "Title"
    std::string s = val;
    if (s == "Title") return 1;
    if (s.rfind("Heading", 0) == 0) {
        try {
            int lvl = std::stoi(s.substr(7));
            if (lvl >= 1 && lvl <= 6) return lvl;
        } catch (...) {}
    }
    return 0;
}

} // namespace

ExtractionResult DocxExtractor::extract(const std::filesystem::path& abs_path)
{
    ExtractionResult result;

    std::string xml;
    if (!read_zip_entry(abs_path, "word/document.xml", xml)) {
        spdlog::warn("DocxExtractor: no word/document.xml in {}", abs_path.string());
        result.is_binary = true;
        return result;
    }

    pugi::xml_document doc;
    auto parse_result = doc.load_buffer(xml.data(), xml.size());
    if (!parse_result) {
        spdlog::warn("DocxExtractor: XML parse failed in {}: {}",
                     abs_path.string(), parse_result.description());
        result.is_binary = true;
        return result;
    }

    pugi::xml_node body = doc.child("w:document").child("w:body");
    if (!body) {
        // Some validators expect different namespace prefixes; try any root.
        body = doc.document_element().child("w:body");
    }
    if (!body) {
        spdlog::warn("DocxExtractor: no body element in {}", abs_path.string());
        result.is_binary = true;
        return result;
    }

    std::string& text = result.text;
    int current_line = 1;

    for (pugi::xml_node child = body.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();

        if (std::strcmp(name, "w:p") == 0) {
            std::string para;
            append_paragraph_text(child, para);

            int level = heading_level(child);
            if (level > 0) {
                // Trim heading text
                auto l = para.find_first_not_of(" \t\r\n");
                auto r = para.find_last_not_of(" \t\r\n");
                if (l != std::string::npos) {
                    std::string ht = para.substr(l, r - l + 1);
                    if (!ht.empty()) {
                        result.headings.push_back({ level, ht, current_line });
                    }
                }
            }

            text += para;
            text += '\n';
            current_line += 1 + static_cast<int>(
                std::count(para.begin(), para.end(), '\n'));
        } else if (std::strcmp(name, "w:tbl") == 0) {
            // Tables: concatenate cell text with tabs / newlines
            for (pugi::xml_node row : child.children("w:tr")) {
                for (pugi::xml_node cell : row.children("w:tc")) {
                    std::string cell_text;
                    append_paragraph_text(cell, cell_text);
                    text += cell_text;
                    text += '\t';
                }
                text += '\n';
                ++current_line;
            }
        }
    }

    if (text.empty()) {
        result.is_binary = true;
    }

    return result;
}

} // namespace locus
