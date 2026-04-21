#include <catch2/catch_test_macros.hpp>

#include "extractors/docx_extractor.h"
#include "extractors/html_extractor.h"
#include "extractors/pdf_extractor.h"
#include "extractors/xlsx_extractor.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace locus;

// The sample_docs folder lives beside this .cpp at repo-source time, but the
// test binary runs from build/<cfg>/bin/.  Walk up from CWD to find tests/.
static fs::path find_sample(const char* name)
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

TEST_CASE("PdfiumExtractor reads IRS W-9 sample", "[s2.3][pdf]")
{
    auto path = find_sample("sample.pdf");
    REQUIRE(!path.empty());

    PdfiumExtractor ext;
    auto r = ext.extract(path);

    REQUIRE_FALSE(r.is_binary);
    REQUIRE(!r.text.empty());
    // W-9 always contains these literal strings
    REQUIRE(r.text.find("W-9") != std::string::npos);
    REQUIRE(r.text.find("Taxpayer") != std::string::npos);
    // At least one page → at least one pseudo-heading
    REQUIRE(!r.headings.empty());
    REQUIRE(r.headings[0].text.rfind("Page ", 0) == 0);
}

TEST_CASE("PdfiumExtractor flags missing file", "[s2.3][pdf]")
{
    PdfiumExtractor ext;
    auto r = ext.extract("nonexistent.pdf");
    REQUIRE(r.is_binary);
}

TEST_CASE("DocxExtractor reads POI SampleDoc", "[s2.3][docx]")
{
    auto path = find_sample("sample.docx");
    REQUIRE(!path.empty());

    DocxExtractor ext;
    auto r = ext.extract(path);

    REQUIRE_FALSE(r.is_binary);
    REQUIRE(!r.text.empty());
}

TEST_CASE("DocxExtractor flags non-zip file", "[s2.3][docx]")
{
    auto tmp = fs::temp_directory_path() / "locus_test_fake.docx";
    {
        std::ofstream f(tmp);
        f << "this is not a zip";
    }
    DocxExtractor ext;
    auto r = ext.extract(tmp);
    REQUIRE(r.is_binary);
    fs::remove(tmp);
}

TEST_CASE("XlsxExtractor reads POI SampleSS", "[s2.3][xlsx]")
{
    auto path = find_sample("sample.xlsx");
    REQUIRE(!path.empty());

    XlsxExtractor ext;
    auto r = ext.extract(path);

    REQUIRE_FALSE(r.is_binary);
    REQUIRE(!r.text.empty());
    // At least one sheet → heading emitted
    REQUIRE(!r.headings.empty());
}

TEST_CASE("HtmlExtractor strips tags and decodes entities", "[s2.3][html]")
{
    auto tmp = fs::temp_directory_path() / "locus_html_test.html";
    {
        std::ofstream f(tmp);
        f << "<html><head><style>body{color:red}</style></head>"
             "<body><h1>Title</h1><p>Hello&nbsp;&amp;&nbsp;goodbye</p>"
             "<script>var x=1;</script></body></html>";
    }

    HtmlExtractor ext;
    auto r = ext.extract(tmp);

    REQUIRE_FALSE(r.is_binary);
    REQUIRE(r.text.find("<") == std::string::npos);
    REQUIRE(r.text.find("Hello") != std::string::npos);
    REQUIRE(r.text.find("goodbye") != std::string::npos);
    REQUIRE(r.text.find("&amp;") == std::string::npos);
    REQUIRE(r.text.find("var x") == std::string::npos);  // script stripped
    REQUIRE(r.text.find("color:red") == std::string::npos);  // style stripped

    REQUIRE(r.headings.size() == 1);
    REQUIRE(r.headings[0].level == 1);
    REQUIRE(r.headings[0].text == "Title");

    fs::remove(tmp);
}
