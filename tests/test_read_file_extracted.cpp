// ReadFileTool extension for PDF/DOCX/XLSX (post-ADR-0009 follow-up).
//
// Today the tool branches on extension: raw bytes for code / Markdown / HTML
// / plain text, and `IndexQuery::get_extracted_text()` (which reads
// `files_fts.content`) for PDF/DOCX/XLSX. The same offset/length window
// applies to the extracted lines. For PDFs each pseudo-line is one page.
//
// These tests copy the same sample fixtures used by test_extractors.cpp into
// a temp workspace, open the workspace (ctor's synchronous initial index pulls
// the samples through the matching extractor), then call ReadFileTool.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "core/workspace.h"
#include "tools/file_tools.h"
#include "tools/tool.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path make_test_dir(const std::string& tag)
{
    auto p = fs::temp_directory_path() / ("locus_test_readfile_ext_" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

// Mirror of test_extractors.cpp's find_sample helper.
fs::path find_sample(const char* name)
{
    fs::path base = fs::current_path();
    for (int i = 0; i < 6; ++i) {
        fs::path candidate = base / "tests" / "sample_docs" / name;
        if (fs::exists(candidate)) return candidate;
        if (!base.has_parent_path() || base.parent_path() == base) break;
        base = base.parent_path();
    }
    return {};
}

locus::ToolResult invoke_read(locus::Workspace& ws,
                              const std::string& path,
                              int offset = 1,
                              int length = 100)
{
    locus::IWorkspaceServices& services = ws;
    locus::ReadFileTool tool;
    nlohmann::json args;
    args["path"] = path;
    args["offset"] = offset;
    args["length"] = length;
    locus::ToolCall call{"c", "read_file", args};
    return tool.execute(call, services);
}

} // namespace

// -- 1. PDF: paginates extractor output -------------------------------------

TEST_CASE("ReadFileTool: PDF returns extracted page-text, not raw bytes",
          "[read_file][extracted][pdf]")
{
    auto sample = find_sample("sample.pdf");
    if (sample.empty()) {
        WARN("sample.pdf missing; skipping");
        return;
    }

    auto tmp = make_test_dir("pdf");
    fs::copy_file(sample, tmp / "spec.pdf");

    {
        locus::Workspace ws(tmp);
        // First page should land in lines 1..N (N >= 1).
        auto r = invoke_read(ws, "spec.pdf", /*offset=*/1, /*length=*/5);
        REQUIRE(r.success);
        INFO("read body:\n" << r.content);

        // Header advertises the extracted-from origin + the per-page hint.
        REQUIRE_THAT(r.content, ContainsSubstring("extracted from .pdf"));
        REQUIRE_THAT(r.content, ContainsSubstring("one line per page"));

        // The W-9 sample's title text lands in the first page's pseudo-line.
        REQUIRE_THAT(r.content, ContainsSubstring("W-9"));

        // Raw byte signature "%PDF-" must NOT leak into the response -- proves
        // we hit the extracted path, not the std::ifstream fallback.
        REQUIRE(r.content.find("%PDF-") == std::string::npos);
    }
    fs::remove_all(tmp);
}

// -- 2. DOCX: paginates extractor output ------------------------------------

TEST_CASE("ReadFileTool: DOCX returns extracted paragraphs",
          "[read_file][extracted][docx]")
{
    auto sample = find_sample("sample.docx");
    if (sample.empty()) {
        WARN("sample.docx missing; skipping");
        return;
    }

    auto tmp = make_test_dir("docx");
    fs::copy_file(sample, tmp / "doc.docx");

    {
        locus::Workspace ws(tmp);
        auto r = invoke_read(ws, "doc.docx");
        REQUIRE(r.success);
        INFO("read body:\n" << r.content);

        REQUIRE_THAT(r.content, ContainsSubstring("extracted from .docx"));
        // The Tika DOCX sample carries "Heading Level 1" in its body
        // (the same string test_extractors.cpp asserts against).
        REQUIRE_THAT(r.content, ContainsSubstring("Heading Level 1"));
        // No OOXML zip signature should leak.
        REQUIRE(r.content.find("PK\x03\x04") == std::string::npos);
    }
    fs::remove_all(tmp);
}

// -- 3. XLSX: paginates extractor output ------------------------------------

TEST_CASE("ReadFileTool: XLSX returns extracted sheet text",
          "[read_file][extracted][xlsx]")
{
    auto sample = find_sample("sample.xlsx");
    if (sample.empty()) {
        WARN("sample.xlsx missing; skipping");
        return;
    }

    auto tmp = make_test_dir("xlsx");
    fs::copy_file(sample, tmp / "data.xlsx");

    {
        locus::Workspace ws(tmp);
        auto r = invoke_read(ws, "data.xlsx");
        REQUIRE(r.success);
        INFO("read body:\n" << r.content);

        REQUIRE_THAT(r.content, ContainsSubstring("extracted from .xlsx"));
        // Body should be non-empty extracted text -- the XLSX extractor
        // produces sheet headings + cell strings; we don't pin the exact
        // string (varies by sample) but it must not be raw zip bytes.
        REQUIRE(r.content.find("PK\x03\x04") == std::string::npos);
    }
    fs::remove_all(tmp);
}

// -- 4. Code/Markdown still go through the raw-bytes path -------------------

TEST_CASE("ReadFileTool: code files keep the raw-bytes path",
          "[read_file][raw]")
{
    auto tmp = make_test_dir("raw");
    {
        std::ofstream f(tmp / "five.cpp", std::ios::binary);
        f << "1\n2\n3\n4\n5\n";
    }
    {
        locus::Workspace ws(tmp);
        auto r = invoke_read(ws, "five.cpp");
        REQUIRE(r.success);
        INFO("read body:\n" << r.content);
        // The raw path's header does NOT carry "extracted from".
        REQUIRE(r.content.find("extracted from") == std::string::npos);
        // Standard line-numbered render.
        REQUIRE_THAT(r.content, ContainsSubstring("1\t1"));
        REQUIRE_THAT(r.content, ContainsSubstring("5\t5"));
        REQUIRE_THAT(r.content, ContainsSubstring("of 5"));
    }
    fs::remove_all(tmp);
}

// -- 5. Extracted-format path errors when the file isn't indexed ------------

TEST_CASE("ReadFileTool: PDF that doesn't exist returns actionable error",
          "[read_file][extracted][error]")
{
    auto tmp = make_test_dir("notindexed");
    {
        locus::Workspace ws(tmp);
        // No PDF on disk + no row in the index -> we should error with the
        // "not yet in the index" message, not fall through to std::ifstream
        // and dump garbage or a generic "cannot open" line.
        auto r = invoke_read(ws, "missing.pdf");
        REQUIRE_FALSE(r.success);
        INFO("error body:\n" << r.content);
        REQUIRE_THAT(r.content, ContainsSubstring("not yet in the index"));
    }
    fs::remove_all(tmp);
}

// -- 6. Pagination: offset overshoot reports total extracted-line count -----

TEST_CASE("ReadFileTool: PDF offset overshoot reports total page count",
          "[read_file][extracted][pdf][pagination]")
{
    auto sample = find_sample("sample.pdf");
    if (sample.empty()) {
        WARN("sample.pdf missing; skipping");
        return;
    }

    auto tmp = make_test_dir("overshoot");
    fs::copy_file(sample, tmp / "spec.pdf");

    {
        locus::Workspace ws(tmp);
        // sample.pdf is at most a few pages; offset=999 always overshoots.
        auto r = invoke_read(ws, "spec.pdf", /*offset=*/999, /*length=*/1);
        REQUIRE_FALSE(r.success);
        INFO("error body:\n" << r.content);
        REQUIRE_THAT(r.content, ContainsSubstring("exceeds file length"));
        REQUIRE_THAT(r.content, ContainsSubstring("lines)"));
    }
    fs::remove_all(tmp);
}
