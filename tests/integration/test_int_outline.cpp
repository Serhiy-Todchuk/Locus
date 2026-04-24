#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace locus::integration;

namespace {

// Ask the LLM to outline a specific sample document and verify it invoked the
// outline tool and got back a non-empty result. Keep assertions loose on the
// display format — we just want evidence the extractor produced headings.
void check_outline_of(const std::string& rel_path, std::string_view marker)
{
    auto& h = harness();
    PromptResult r = h.prompt(
        "Use the get_file_outline tool on `" + rel_path + "` and summarise what "
        "headings or sections it contains.");

    INFO("file: " << rel_path);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("get_file_outline"));

    const auto* res = r.find_result_for("get_file_outline");
    REQUIRE(res != nullptr);
    REQUIRE_FALSE(res->display.empty());

    // Every extractor format produces at least one heading/pseudo-heading.
    INFO("display:\n" << res->display);
    REQUIRE(res->display.find(marker) != std::string::npos);
}

} // namespace

TEST_CASE("outline works on markdown sample", "[integration][llm][outline]")
{
    check_outline_of("tests/sample_docs/README.md", "Sample Documents");
}

TEST_CASE("outline works on PDF sample (PDFium)", "[integration][llm][outline]")
{
    // PDFium extractor emits a pseudo-heading per page, so any outline text
    // will contain "page" in some form. Keep the marker coarse.
    check_outline_of("tests/sample_docs/sample.pdf", "age ");  // "page ", "Page "
}

TEST_CASE("outline works on DOCX sample", "[integration][llm][outline]")
{
    // The Apache POI SampleDoc.docx has no explicit HeadingN styles so its
    // outline is empty — we only check that the extractor ran (tool produced
    // a result with the path header) rather than asserting a specific marker.
    check_outline_of("tests/sample_docs/sample.docx", "sample.docx");
}

TEST_CASE("outline works on XLSX sample", "[integration][llm][outline]")
{
    // XlsxExtractor emits one pseudo-heading per sheet ("Sheet1", etc.).
    check_outline_of("tests/sample_docs/sample.xlsx", "heet");  // "Sheet"
}
