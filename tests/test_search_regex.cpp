// S4.P — regex mode on the unified `search` tool.
//
// These tests drive the real `Workspace` so the indexer's file enumeration
// (which regex mode consumes via `list_directory`) is exercised end-to-end.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "tools/tool.h"
#include "tools/search_tools.h"
#include "core/workspace.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path make_test_dir(const char* suffix)
{
    auto tmp = fs::temp_directory_path() / ("locus_test_regex_" + std::string(suffix));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    return tmp;
}

void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << content;
}

void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

locus::ToolCall regex_call(const nlohmann::json& args)
{
    return {"r1", "search", args};
}

} // namespace

// -- Dispatch ---------------------------------------------------------------

TEST_CASE("search mode=regex dispatches to regex backend", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("dispatch");
    write_file(tmp / "foo.cpp", "int x = 42;\nreturn x;");

    {
        locus::Workspace ws(tmp);

        locus::SearchTool tool;
        auto result = tool.execute(regex_call({
            {"mode", "regex"},
            {"query", "int\\s+\\w+"},
        }), ws);

        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("foo.cpp:1"));
    }
    cleanup(tmp);
}

TEST_CASE("search mode=regex requires query", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("needs_query");
    write_file(tmp / "a.txt", "hello");

    {
        locus::Workspace ws(tmp);
        locus::SearchTool tool;
        auto result = tool.execute(regex_call({{"mode", "regex"}}), ws);

        REQUIRE_FALSE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("query"));
    }
    cleanup(tmp);
}

TEST_CASE("search mode=regex rejects malformed pattern", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("bad_pattern");
    write_file(tmp / "a.txt", "hello");

    {
        locus::Workspace ws(tmp);
        locus::SearchTool tool;
        // Unclosed group — invalid ECMAScript regex.
        auto result = tool.execute(regex_call({
            {"mode", "regex"},
            {"query", "foo("},
        }), ws);

        REQUIRE_FALSE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("invalid regex"));
    }
    cleanup(tmp);
}

// -- Special characters (the whole point of regex vs FTS5) ------------------

TEST_CASE("search regex matches punctuation-heavy identifiers", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("punct");
    write_file(tmp / "cache.cpp",
        "void foo() {\n"
        "    auto v = obj->m_cache;\n"
        "    // TODO(XXX): replace m_cache with shared_ptr\n"
        "    return v;\n"
        "}\n");
    write_file(tmp / "other.cpp", "int unrelated = 0;\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchRegexTool tool;

        // Arrow + member — FTS5 would lose the punctuation entirely.
        auto arrow = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "->m_cache"}}}, ws);
        REQUIRE(arrow.success);
        REQUIRE_THAT(arrow.content, ContainsSubstring("cache.cpp:2"));

        // TODO marker with parenthesised tag.
        auto todo = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "TODO\\(XXX\\)"}}}, ws);
        REQUIRE(todo.success);
        REQUIRE_THAT(todo.content, ContainsSubstring("cache.cpp:3"));
    }
    cleanup(tmp);
}

// -- Case sensitivity --------------------------------------------------------

TEST_CASE("search regex is case-sensitive by default", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("case_default");
    write_file(tmp / "a.txt", "Hello World\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchRegexTool tool;

        auto hit = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "Hello"}}}, ws);
        REQUIRE(hit.success);
        REQUIRE_THAT(hit.content, ContainsSubstring("a.txt:1"));

        auto miss = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "hello"}}}, ws);
        REQUIRE(miss.success);
        REQUIRE_THAT(miss.content, ContainsSubstring("0 matches"));
    }
    cleanup(tmp);
}

TEST_CASE("search regex case_sensitive=false finds mixed-case", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("case_off");
    write_file(tmp / "a.txt", "HELLO world\nhello Again\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchRegexTool tool;

        auto hit = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "hello"},
                 {"case_sensitive", false}}}, ws);
        REQUIRE(hit.success);
        REQUIRE_THAT(hit.content, ContainsSubstring("2 matches"));
        REQUIRE_THAT(hit.content, ContainsSubstring("a.txt:1"));
        REQUIRE_THAT(hit.content, ContainsSubstring("a.txt:2"));
    }
    cleanup(tmp);
}

// -- Multi-line patterns -----------------------------------------------------

TEST_CASE("search regex can span multiple lines via [\\s\\S]", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("multiline");
    write_file(tmp / "block.cpp",
        "class Foo {\n"
        "public:\n"
        "    void bar();\n"
        "};\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchRegexTool tool;

        // `.` does not match newlines in ECMAScript; [\s\S] is the idiom.
        auto hit = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "class Foo[\\s\\S]*?bar\\(\\)"}}}, ws);
        REQUIRE(hit.success);
        REQUIRE_THAT(hit.content, ContainsSubstring("block.cpp:1"));
    }
    cleanup(tmp);
}

// -- Output shape: line numbers + context ------------------------------------

TEST_CASE("search regex output includes line numbers and 1-line context", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("output");
    write_file(tmp / "multi.txt",
        "line one\n"
        "target line here\n"
        "line three\n"
        "line four\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchRegexTool tool;

        auto result = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "target"}}}, ws);
        REQUIRE(result.success);
        // Line number appears.
        REQUIRE_THAT(result.content, ContainsSubstring("multi.txt:2"));
        // Context above and below the match should both be present.
        REQUIRE_THAT(result.content, ContainsSubstring("line one"));
        REQUIRE_THAT(result.content, ContainsSubstring("target line here"));
        REQUIRE_THAT(result.content, ContainsSubstring("line three"));
    }
    cleanup(tmp);
}

// -- path_glob filter --------------------------------------------------------

TEST_CASE("search regex path_glob narrows the file set", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("glob");
    write_file(tmp / "keep.cpp", "needle\n");
    write_file(tmp / "skip.txt", "needle\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchRegexTool tool;

        auto result = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "needle"},
                 {"path_glob", "*.cpp"}}}, ws);
        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("keep.cpp"));
        REQUIRE_THAT(result.content, !ContainsSubstring("skip.txt"));
    }
    cleanup(tmp);
}

// -- max_results cap ---------------------------------------------------------

TEST_CASE("search regex respects max_results", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("cap");
    std::string content;
    for (int i = 0; i < 30; ++i) content += "hit here\n";
    write_file(tmp / "many.txt", content);

    {
        locus::Workspace ws(tmp);
        locus::SearchRegexTool tool;

        auto result = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "hit"},
                 {"max_results", 5}}}, ws);
        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("5 matches"));
        REQUIRE_THAT(result.content, ContainsSubstring("truncated"));
    }
    cleanup(tmp);
}

// -- Binary files are skipped ------------------------------------------------

TEST_CASE("search regex skips binary files", "[s4.p][search][regex]")
{
    auto tmp = make_test_dir("binary");
    // Null byte within the first kilobyte makes the indexer mark it binary.
    std::string binary_blob;
    binary_blob.push_back('M');
    binary_blob.push_back('Z');
    binary_blob.push_back('\0');
    binary_blob += "needle inside a binary file";
    write_file(tmp / "blob.bin", binary_blob);
    write_file(tmp / "text.txt", "needle in text\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchRegexTool tool;

        auto result = tool.execute(
            locus::ToolCall{"r", "search_regex",
                {{"query", "needle"}}}, ws);
        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("text.txt"));
        REQUIRE_THAT(result.content, !ContainsSubstring("blob.bin"));
    }
    cleanup(tmp);
}

// -- Schema --------------------------------------------------------------------

TEST_CASE("search tool exposes regex params in its schema", "[s4.p][search][regex]")
{
    locus::SearchTool tool;
    auto params = tool.params();

    bool has_query = false, has_glob = false, has_case = false, has_mode = false;
    bool has_pattern_leftover = false;
    for (auto& p : params) {
        if (p.name == "query")          has_query   = true;
        if (p.name == "path_glob")      has_glob    = true;
        if (p.name == "case_sensitive") has_case    = true;
        if (p.name == "mode")           has_mode    = true;
        if (p.name == "pattern")        has_pattern_leftover = true;
    }
    REQUIRE(has_query);
    REQUIRE(has_glob);
    REQUIRE(has_case);
    REQUIRE(has_mode);
    // Regex mode reuses `query`; the legacy `pattern` arg is gone.
    REQUIRE_FALSE(has_pattern_leftover);

    REQUIRE_THAT(tool.description(), ContainsSubstring("regex"));
}
