#include <catch2/catch_test_macros.hpp>

#include "frontends/gui/diff_renderer.h"

#include <nlohmann/json.hpp>

#include <string>

using locus::DiffRenderOptions;
using locus::render_edit_file_diff_html;
using locus::render_write_file_diff_html;
using locus::render_delete_file_summary_html;
using locus::html_escape_for_diff;
using nlohmann::json;

namespace {

// Test helper: does `haystack` contain `needle` as a substring?
bool has(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Count occurrences of `needle` in `haystack`.
int count_occurrences(const std::string& haystack, const std::string& needle)
{
    int n = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

TEST_CASE("HTML escape handles the dangerous five", "[s5.c][diff]")
{
    REQUIRE(html_escape_for_diff("<tag>") == "&lt;tag&gt;");
    REQUIRE(html_escape_for_diff("a & b") == "a &amp; b");
    REQUIRE(html_escape_for_diff("\"quoted\"") == "&quot;quoted&quot;");
    // \r is stripped (CRLF -> LF visual equivalence).
    REQUIRE(html_escape_for_diff("line\r\n") == "line\n");
}

TEST_CASE("edit_file: single edit emits one del + one add line", "[s5.c][diff][edit_file]")
{
    json args = {
        {"path", "src/foo.cpp"},
        {"edits", json::array({
            json{{"old_string", "old line"}, {"new_string", "new line"}}
        })}
    };

    DiffRenderOptions opts;
    auto html = render_edit_file_diff_html(args, opts);

    REQUIRE(has(html, "edit_file:"));
    REQUIRE(has(html, "src/foo.cpp"));
    REQUIRE(has(html, "@@ edit 1/1 @@"));
    REQUIRE(has(html, "diff-line del\">old line</div>"));
    REQUIRE(has(html, "diff-line add\">new line</div>"));
}

TEST_CASE("edit_file: batched edits produce multiple hunks in order", "[s5.c][diff][edit_file]")
{
    json args = {
        {"path", "x.txt"},
        {"edits", json::array({
            json{{"old_string", "a"}, {"new_string", "AAA"}},
            json{{"old_string", "b"}, {"new_string", "BBB"}, {"replace_all", true}},
            json{{"old_string", "c"}, {"new_string", "CCC"}}
        })}
    };

    auto html = render_edit_file_diff_html(args, {});
    REQUIRE(count_occurrences(html, "diff-hunk-header") == 3);
    REQUIRE(has(html, "@@ edit 1/3 @@"));
    REQUIRE(has(html, "@@ edit 2/3 (replace_all) @@"));
    REQUIRE(has(html, "@@ edit 3/3 @@"));

    // Ordering: hunk 1 appears before hunk 2 in the output.
    auto p1 = html.find("@@ edit 1/3");
    auto p2 = html.find("@@ edit 2/3");
    auto p3 = html.find("@@ edit 3/3");
    REQUIRE(p1 < p2);
    REQUIRE(p2 < p3);
}

TEST_CASE("edit_file: malformed args yields empty output", "[s5.c][diff][edit_file]")
{
    REQUIRE(render_edit_file_diff_html(json::object(),     {}).empty());
    REQUIRE(render_edit_file_diff_html(json::array(),      {}).empty());
    REQUIRE(render_edit_file_diff_html(json{{"edits", json::array()}}, {}).empty());
}

TEST_CASE("edit_file: multi-line old/new_string splits into N diff lines",
          "[s5.c][diff][edit_file]")
{
    json args = {
        {"path", "x.cpp"},
        {"edits", json::array({
            json{{"old_string", "line1\nline2\nline3"},
                 {"new_string", "alpha\nbeta"}}
        })}
    };
    auto html = render_edit_file_diff_html(args, {});
    // 3 del lines + 2 add lines
    REQUIRE(count_occurrences(html, "diff-line del") == 3);
    REQUIRE(count_occurrences(html, "diff-line add") == 2);
    REQUIRE(has(html, ">line1</div>"));
    REQUIRE(has(html, ">line2</div>"));
    REQUIRE(has(html, ">line3</div>"));
    REQUIRE(has(html, ">alpha</div>"));
    REQUIRE(has(html, ">beta</div>"));
}

TEST_CASE("edit_file: HTML in args is escaped", "[s5.c][diff][edit_file]")
{
    json args = {
        {"path", "<scary>.cpp"},
        {"edits", json::array({
            json{{"old_string", "<script>alert(1)</script>"},
                 {"new_string", "safe & sound"}}
        })}
    };
    auto html = render_edit_file_diff_html(args, {});
    REQUIRE(!has(html, "<script>alert(1)</script>"));
    REQUIRE(has(html, "&lt;script&gt;alert(1)&lt;/script&gt;"));
    REQUIRE(has(html, "safe &amp; sound"));
    REQUIRE(has(html, "&lt;scary&gt;.cpp"));
}

TEST_CASE("edit_file: max_lines cap inserts truncation marker", "[s5.c][diff][edit_file]")
{
    // 50 edits * (1 header + 1 del + 1 add) = 150 lines; cap at 30
    json edits = json::array();
    for (int i = 0; i < 50; ++i) {
        edits.push_back(json{{"old_string", "o"}, {"new_string", "n"}});
    }
    json args = {{"path", "x"}, {"edits", edits}};

    DiffRenderOptions opts;
    opts.max_lines = 30;
    auto html = render_edit_file_diff_html(args, opts);

    REQUIRE(has(html, "diff-truncated"));
    REQUIRE(has(html, "not shown"));
}

TEST_CASE("write_file: nullopt old_content -> all-add", "[s5.c][diff][write_file]")
{
    auto html = render_write_file_diff_html(
        "new/file.cpp",
        std::nullopt,
        "first\nsecond\nthird\n",
        {});
    REQUIRE(has(html, "write_file:"));
    REQUIRE(has(html, "new/file.cpp"));
    REQUIRE(count_occurrences(html, "diff-line add") == 3);
    REQUIRE(count_occurrences(html, "diff-line del") == 0);
}

TEST_CASE("write_file: identical before/after renders no-changes", "[s5.c][diff][write_file]")
{
    std::string body = "alpha\nbeta\ngamma\n";
    auto html = render_write_file_diff_html("x.txt", body, body, {});
    REQUIRE(has(html, "(no changes)"));
    REQUIRE(count_occurrences(html, "diff-line add") == 0);
    REQUIRE(count_occurrences(html, "diff-line del") == 0);
}

TEST_CASE("write_file: minimal diff via dtl LCS", "[s5.c][diff][write_file]")
{
    // Insert one line in the middle: SES should be COMMON, COMMON, ADD, COMMON.
    std::string old_s = "a\nb\nc\n";
    std::string new_s = "a\nb\nNEW\nc\n";
    auto html = render_write_file_diff_html("x", old_s, new_s, {});

    REQUIRE(count_occurrences(html, "diff-line add") == 1);
    REQUIRE(count_occurrences(html, "diff-line del") == 0);
    REQUIRE(count_occurrences(html, "diff-line ctx") == 3);
    REQUIRE(has(html, ">NEW</div>"));
}

TEST_CASE("write_file: line deletion via dtl LCS", "[s5.c][diff][write_file]")
{
    std::string old_s = "a\nb\nc\nd\n";
    std::string new_s = "a\nc\nd\n";
    auto html = render_write_file_diff_html("x", old_s, new_s, {});
    REQUIRE(count_occurrences(html, "diff-line del") == 1);
    REQUIRE(count_occurrences(html, "diff-line add") == 0);
    REQUIRE(has(html, ">b</div>"));
}

TEST_CASE("write_file: max_lines truncates with footer", "[s5.c][diff][write_file]")
{
    std::string body;
    for (int i = 0; i < 500; ++i) body += "line" + std::to_string(i) + "\n";
    DiffRenderOptions opts;
    opts.max_lines = 50;
    auto html = render_write_file_diff_html("big.txt", std::nullopt, body, opts);
    REQUIRE(has(html, "diff-truncated"));
    REQUIRE(count_occurrences(html, "diff-line add") <= 50);
}

TEST_CASE("delete_file: emits summary with line count", "[s5.c][diff][delete_file]")
{
    auto html = render_delete_file_summary_html("docs/old.md", 42);
    REQUIRE(has(html, "delete_file:"));
    REQUIRE(has(html, "docs/old.md"));
    REQUIRE(has(html, "(42 lines)"));
    REQUIRE(has(html, "diff-deleted"));
    // Singular grammar for 1.
    auto html1 = render_delete_file_summary_html("x", 1);
    REQUIRE(has(html1, "(1 line)"));
    REQUIRE(!has(html1, "(1 lines)"));
}

TEST_CASE("delete_file: zero line count omits the count", "[s5.c][diff][delete_file]")
{
    auto html = render_delete_file_summary_html("unknown.bin", 0);
    REQUIRE(has(html, "delete_file:"));
    REQUIRE(has(html, "unknown.bin"));
    REQUIRE(!has(html, "lines)"));
    REQUIRE(!has(html, "line)"));
}
