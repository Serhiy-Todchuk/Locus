#include "xlsx_extractor.h"
#include "zip_reader.h"

#include <spdlog/spdlog.h>

#include <miniz.h>
#include <pugixml.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace locus {

namespace {

// Load shared string table from xl/sharedStrings.xml.
// Each <si> has either a single <t> or multiple <r><t> runs (rich text).
std::vector<std::string> load_shared_strings(const std::filesystem::path& xlsx_path)
{
    std::vector<std::string> result;
    std::string xml;
    if (!read_zip_entry(xlsx_path, "xl/sharedStrings.xml", xml)) {
        return result;  // optional part
    }

    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) {
        return result;
    }

    for (pugi::xml_node si : doc.child("sst").children("si")) {
        std::string s;
        // Simple case: <si><t>text</t></si>
        if (pugi::xml_node t = si.child("t")) {
            s = t.text().as_string();
        } else {
            // Rich text: concatenate all <r><t>
            for (pugi::xml_node r : si.children("r")) {
                if (pugi::xml_node rt = r.child("t")) {
                    s += rt.text().as_string();
                }
            }
        }
        result.push_back(std::move(s));
    }

    return result;
}

// Extract (sheet_name, relative_target) pairs from xl/workbook.xml and rels.
struct SheetInfo {
    std::string name;
    std::string target;  // e.g. "worksheets/sheet1.xml"
};

std::vector<SheetInfo> list_sheets(const std::filesystem::path& xlsx_path)
{
    std::vector<SheetInfo> out;

    std::string wb_xml;
    if (!read_zip_entry(xlsx_path, "xl/workbook.xml", wb_xml)) {
        return out;
    }
    pugi::xml_document wb;
    if (!wb.load_buffer(wb_xml.data(), wb_xml.size())) {
        return out;
    }

    // Build rId → target map from xl/_rels/workbook.xml.rels
    std::string rels_xml;
    read_zip_entry(xlsx_path, "xl/_rels/workbook.xml.rels", rels_xml);

    std::vector<std::pair<std::string, std::string>> rid_to_target;
    if (!rels_xml.empty()) {
        pugi::xml_document rels;
        if (rels.load_buffer(rels_xml.data(), rels_xml.size())) {
            for (pugi::xml_node rel : rels.child("Relationships").children("Relationship")) {
                rid_to_target.emplace_back(
                    rel.attribute("Id").value(),
                    rel.attribute("Target").value());
            }
        }
    }

    auto find_target = [&](const std::string& rid) -> std::string {
        for (auto& [id, tgt] : rid_to_target)
            if (id == rid) return tgt;
        return "";
    };

    for (pugi::xml_node sheet : wb.child("workbook").child("sheets").children("sheet")) {
        SheetInfo info;
        info.name = sheet.attribute("name").value();
        std::string rid = sheet.attribute("r:id").value();
        if (rid.empty()) rid = sheet.attribute("id").value();  // defensive
        std::string target = find_target(rid);
        if (target.empty()) continue;

        // Normalise: rels target is relative to xl/
        if (target.rfind("/", 0) == 0) {
            target = target.substr(1);  // absolute path in zip
        } else {
            target = "xl/" + target;
        }
        info.target = target;
        out.push_back(std::move(info));
    }

    return out;
}

// Append all text from a single sheet XML to `out`.  Numbers written as-is.
// Strings resolve via shared_strings when cell type is "s".
void append_sheet_text(const std::string& sheet_xml,
                       const std::vector<std::string>& shared_strings,
                       std::string& out)
{
    pugi::xml_document doc;
    if (!doc.load_buffer(sheet_xml.data(), sheet_xml.size())) return;

    pugi::xml_node sheet_data = doc.child("worksheet").child("sheetData");
    if (!sheet_data) return;

    for (pugi::xml_node row : sheet_data.children("row")) {
        bool first_cell = true;
        for (pugi::xml_node c : row.children("c")) {
            if (!first_cell) out += '\t';
            first_cell = false;

            const char* type = c.attribute("t").value();
            pugi::xml_node v = c.child("v");

            if (std::strcmp(type, "s") == 0) {
                // Shared string index
                if (v) {
                    try {
                        size_t idx = static_cast<size_t>(std::stoul(v.text().as_string()));
                        if (idx < shared_strings.size())
                            out += shared_strings[idx];
                    } catch (...) {}
                }
            } else if (std::strcmp(type, "inlineStr") == 0) {
                if (pugi::xml_node is = c.child("is")) {
                    if (pugi::xml_node t = is.child("t")) {
                        out += t.text().as_string();
                    }
                }
            } else {
                // Number, bool, date, formula result — emit text as-is
                if (v) out += v.text().as_string();
            }
        }
        out += '\n';
    }
}

} // namespace

ExtractionResult XlsxExtractor::extract(const std::filesystem::path& abs_path)
{
    ExtractionResult result;

    auto shared_strings = load_shared_strings(abs_path);
    auto sheets = list_sheets(abs_path);

    if (sheets.empty()) {
        spdlog::warn("XlsxExtractor: no sheets found in {}", abs_path.string());
        result.is_binary = true;
        return result;
    }

    std::string& text = result.text;
    for (const auto& sheet : sheets) {
        int line_number = 1 + static_cast<int>(
            std::count(text.begin(), text.end(), '\n'));
        result.headings.push_back({ 1, sheet.name, line_number });

        text += "# " + sheet.name + "\n";

        std::string sheet_xml;
        if (!read_zip_entry(abs_path, sheet.target, sheet_xml)) {
            spdlog::trace("XlsxExtractor: skipping sheet {} (target {} missing)",
                          sheet.name, sheet.target);
            continue;
        }
        append_sheet_text(sheet_xml, shared_strings, text);
        text += '\n';
    }

    if (text.empty()) {
        result.is_binary = true;
    }

    return result;
}

} // namespace locus
