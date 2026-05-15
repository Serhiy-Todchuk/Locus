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
    opts.context_lines = 0;  // no old_content -> no context anyway
    auto html = render_edit_file_diff_html(args, std::nullopt, opts);

    REQUIRE(has(html, "edit_file:"));
    REQUIRE(has(html, "src/foo.cpp"));
    REQUIRE(has(html, "@@ edit 1/1 @@"));
    // diff-line spans for line numbers come before the visible text; check
    // class + text fragments independently.
    REQUIRE(has(html, "diff-line del"));
    REQUIRE(has(html, ">old line</span>"));
    REQUIRE(has(html, "diff-line add"));
    REQUIRE(has(html, ">new line</span>"));
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

    auto html = render_edit_file_diff_html(args, std::nullopt, {});
    REQUIRE(count_occurrences(html, "diff-hunk-header") == 3);
    REQUIRE(has(html, "@@ edit 1/3 @@"));
    // With no old_content, hunk header doesn't carry a "@ line N" suffix.
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
    REQUIRE(render_edit_file_diff_html(json::object(),     std::nullopt, {}).empty());
    REQUIRE(render_edit_file_diff_html(json::array(),      std::nullopt, {}).empty());
    REQUIRE(render_edit_file_diff_html(json{{"edits", json::array()}},
                                        std::nullopt, {}).empty());
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
    auto html = render_edit_file_diff_html(args, std::nullopt, {});
    // 3 del lines + 2 add lines
    REQUIRE(count_occurrences(html, "diff-line del") == 3);
    REQUIRE(count_occurrences(html, "diff-line add") == 2);
    REQUIRE(has(html, ">line1</span>"));
    REQUIRE(has(html, ">line2</span>"));
    REQUIRE(has(html, ">line3</span>"));
    REQUIRE(has(html, ">alpha</span>"));
    REQUIRE(has(html, ">beta</span>"));
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
    auto html = render_edit_file_diff_html(args, std::nullopt, {});
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
    auto html = render_edit_file_diff_html(args, std::nullopt, opts);

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
    DiffRenderOptions opts;
    opts.context_lines = 4;  // bigger than the file -- nothing gets collapsed
    auto html = render_write_file_diff_html("x", old_s, new_s, opts);

    REQUIRE(count_occurrences(html, "diff-line add") == 1);
    REQUIRE(count_occurrences(html, "diff-line del") == 0);
    REQUIRE(count_occurrences(html, "diff-line ctx") == 3);
    REQUIRE(has(html, ">NEW</span>"));
}

TEST_CASE("write_file: line deletion via dtl LCS", "[s5.c][diff][write_file]")
{
    std::string old_s = "a\nb\nc\nd\n";
    std::string new_s = "a\nc\nd\n";
    DiffRenderOptions opts;
    opts.context_lines = 4;
    auto html = render_write_file_diff_html("x", old_s, new_s, opts);
    REQUIRE(count_occurrences(html, "diff-line del") == 1);
    REQUIRE(count_occurrences(html, "diff-line add") == 0);
    REQUIRE(has(html, ">b</span>"));
}

TEST_CASE("write_file: max_lines truncates with footer", "[s5.c][diff][write_file]")
{
    std::string body;
    for (int i = 0; i < 500; ++i) body += "line" + std::to_string(i) + "\n";
    DiffRenderOptions opts;
    opts.max_lines = 50;
    opts.collapse_threshold = 0;  // isolate the max_lines path from collapse
    auto html = render_write_file_diff_html("big.txt", std::nullopt, body, opts);
    REQUIRE(has(html, "diff-truncated"));
    REQUIRE(count_occurrences(html, "diff-line add") <= 50);
}

TEST_CASE("write_file: collapse threshold folds overflow into <details>",
          "[s5.z][diff][write_file]")
{
    // 30 lines, all-add (new file). Threshold 16 -> first 16 inline, the
    // remaining 14 inside a <details>.
    std::string body;
    for (int i = 1; i <= 30; ++i) body += "ln" + std::to_string(i) + "\n";
    DiffRenderOptions opts;
    opts.collapse_threshold = 16;
    opts.context_lines      = 0;  // doesn't apply on a brand-new file anyway
    auto html = render_write_file_diff_html("f.txt", std::nullopt, body, opts);

    // All 30 add rows are still in the DOM -- the collapse only hides them
    // behind a click, doesn't drop them.
    REQUIRE(count_occurrences(html, "diff-line add") == 30);

    // Toggle exists and advertises the hidden count.
    REQUIRE(has(html, "<details class=\"diff-collapsed\""));
    REQUIRE(has(html, "<summary>Show 14 more lines</summary>"));

    // First 16 rows are NOT inside the <details>; row 17+ ARE.
    auto details_pos = html.find("<details class=\"diff-collapsed\"");
    REQUIRE(details_pos != std::string::npos);
    auto row16 = html.find(">ln16</span>");
    auto row17 = html.find(">ln17</span>");
    REQUIRE(row16 < details_pos);
    REQUIRE(row17 > details_pos);
}

TEST_CASE("write_file: collapse threshold=0 disables collapsing",
          "[s5.z][diff][write_file]")
{
    std::string body;
    for (int i = 1; i <= 30; ++i) body += "ln" + std::to_string(i) + "\n";
    DiffRenderOptions opts;
    opts.collapse_threshold = 0;
    auto html = render_write_file_diff_html("f.txt", std::nullopt, body, opts);
    REQUIRE(!has(html, "diff-collapsed"));
    REQUIRE(count_occurrences(html, "diff-line add") == 30);
}

TEST_CASE("write_file: short diff stays inline (no collapse)",
          "[s5.z][diff][write_file]")
{
    // 8 lines, threshold 16 -> nothing to collapse.
    std::string body;
    for (int i = 1; i <= 8; ++i) body += "ln" + std::to_string(i) + "\n";
    DiffRenderOptions opts;
    opts.collapse_threshold = 16;
    auto html = render_write_file_diff_html("f.txt", std::nullopt, body, opts);
    REQUIRE(!has(html, "diff-collapsed"));
    REQUIRE(count_occurrences(html, "diff-line add") == 8);
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

TEST_CASE("edit_file: with old_content emits line numbers + 4-line context",
          "[s5.z][diff][edit_file]")
{
    // 12-line file; edit replaces line 6. With ctx=4 the renderer should show
    // lines 2..5 as ctx before, line 6 as del, the add line, then 7..10 as
    // ctx after. Lines 1 and 11..12 are NOT shown.
    std::string file;
    for (int i = 1; i <= 12; ++i) file += "line" + std::to_string(i) + "\n";

    json args = {
        {"path", "f.txt"},
        {"edits", json::array({
            json{{"old_string", "line6"}, {"new_string", "LINE6"}}
        })}
    };

    DiffRenderOptions opts;
    opts.context_lines = 4;
    auto html = render_edit_file_diff_html(args, file, opts);

    // Hunk header carries the location now that we know it.
    REQUIRE(has(html, "@@ edit 1/1 @ line 6 @@"));

    // Context-before: lines 2..5. Trailing context: 7..10. NOT lines 1, 11, 12.
    REQUIRE(has(html, ">line2</span>"));
    REQUIRE(has(html, ">line5</span>"));
    REQUIRE(has(html, ">line7</span>"));
    REQUIRE(has(html, ">line10</span>"));
    REQUIRE(!has(html, ">line1</span>"));
    REQUIRE(!has(html, ">line11</span>"));
    REQUIRE(!has(html, ">line12</span>"));

    // The del + add rows themselves.
    REQUIRE(has(html, "diff-line del"));
    REQUIRE(has(html, ">line6</span>"));
    REQUIRE(has(html, "diff-line add"));
    REQUIRE(has(html, ">LINE6</span>"));

    // 8 ctx rows total (4 before + 4 after).
    REQUIRE(count_occurrences(html, "diff-line ctx") == 8);

    // Line-number column for line 6 (right-aligned in 4 chars).
    REQUIRE(has(html, "diff-ln diff-ln-old"));
    REQUIRE(has(html, ">   6</span>"));
}

TEST_CASE("edit_file: context_lines=0 disables context entirely",
          "[s5.z][diff][edit_file]")
{
    std::string file = "a\nb\nc\nFIND\nd\ne\n";
    json args = {
        {"path", "f.txt"},
        {"edits", json::array({
            json{{"old_string", "FIND"}, {"new_string", "FOUND"}}
        })}
    };
    DiffRenderOptions opts;
    opts.context_lines = 0;
    auto html = render_edit_file_diff_html(args, file, opts);
    REQUIRE(count_occurrences(html, "diff-line ctx") == 0);
    REQUIRE(has(html, ">FIND</span>"));
    REQUIRE(has(html, ">FOUND</span>"));
}

TEST_CASE("edit_file: context clamps at start of file",
          "[s5.z][diff][edit_file]")
{
    // Edit hits line 2 -- only one line of pre-context available.
    std::string file = "a\nFIND\nb\nc\nd\ne\nf\n";
    json args = {
        {"path", "f.txt"},
        {"edits", json::array({
            json{{"old_string", "FIND"}, {"new_string", "X"}}
        })}
    };
    DiffRenderOptions opts;
    opts.context_lines = 4;
    auto html = render_edit_file_diff_html(args, file, opts);
    // Only line 1 ('a') survives as before-context; the renderer must not
    // walk off the front of the file.
    REQUIRE(count_occurrences(html, "diff-line ctx") <= 5);  // 1 before + 4 after
    REQUIRE(has(html, ">a</span>"));
}

TEST_CASE("write_file: context_lines=4 collapses interior unchanged runs",
          "[s5.z][diff][write_file]")
{
    // 20 unchanged lines, one add at line 10. With ctx=4 the renderer should
    // show ctx for lines 6..9 and 11..14 (8 rows), plus the ADD; lines 1..5
    // and 15..20 become a "... N unchanged ..." footer.
    std::string old_s;
    std::string new_s;
    for (int i = 1; i <= 20; ++i) {
        std::string base = "ln" + std::to_string(i) + "\n";
        old_s += base;
        new_s += base;
        if (i == 10) new_s += "INSERTED\n";
    }

    DiffRenderOptions opts;
    opts.context_lines = 4;
    auto html = render_write_file_diff_html("x", old_s, new_s, opts);

    REQUIRE(count_occurrences(html, "diff-line add") == 1);
    REQUIRE(has(html, ">INSERTED</span>"));
    // Surrounding ctx rows present: 4 before + 4 after.
    REQUIRE(count_occurrences(html, "diff-line ctx") == 8);
    // Far-away context is collapsed.
    REQUIRE(has(html, "unchanged line"));
    REQUIRE(!has(html, ">ln1</span>"));
    REQUIRE(!has(html, ">ln20</span>"));
}

TEST_CASE("delete_file: zero line count omits the count", "[s5.c][diff][delete_file]")
{
    auto html = render_delete_file_summary_html("unknown.bin", 0);
    REQUIRE(has(html, "delete_file:"));
    REQUIRE(has(html, "unknown.bin"));
    REQUIRE(!has(html, "lines)"));
    REQUIRE(!has(html, "line)"));
}
