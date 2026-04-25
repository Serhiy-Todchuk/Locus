// S4.M -- Tree-sitter structural query tool (`search` mode=ast).
//
// These tests open a real `Workspace` so the indexer runs over the temp dir
// and the AST search reads from the populated `files` table.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "tool.h"
#include "tools/search_tools.h"
#include "workspace.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path make_test_dir(const char* suffix)
{
    auto tmp = fs::temp_directory_path() / ("locus_test_ast_" + std::string(suffix));
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

locus::ToolCall ast_call(const nlohmann::json& args)
{
    return {"a1", "search", args};
}

} // namespace

// -- Dispatch ---------------------------------------------------------------

TEST_CASE("search mode=ast dispatches to ast backend", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("dispatch");
    write_file(tmp / "a.cpp",
        "void f() {\n"
        "    int* p = malloc(8);\n"
        "}\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchTool tool;

        auto result = tool.execute(ast_call({
            {"mode", "ast"},
            {"language", "cpp"},
            {"query", "(call_expression function: (identifier) @fn)"},
        }), ws);

        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("a.cpp:2"));
        REQUIRE_THAT(result.content, ContainsSubstring("@fn"));
    }
    cleanup(tmp);
}

// -- Required parameters ----------------------------------------------------

TEST_CASE("search mode=ast requires language", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("needs_lang");
    write_file(tmp / "a.cpp", "int x = 0;\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;
        auto result = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"query", "(identifier) @id"}}}, ws);
        REQUIRE_FALSE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("language"));
    }
    cleanup(tmp);
}

TEST_CASE("search mode=ast requires query", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("needs_query");
    write_file(tmp / "a.cpp", "int x = 0;\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;
        auto result = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"}}}, ws);
        REQUIRE_FALSE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("query"));
    }
    cleanup(tmp);
}

TEST_CASE("search mode=ast rejects unknown language", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("bad_lang");
    write_file(tmp / "a.cpp", "int x = 0;\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;
        auto result = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "haskell"},
                 {"query", "(identifier) @id"}}}, ws);
        REQUIRE_FALSE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("unsupported language"));
    }
    cleanup(tmp);
}

TEST_CASE("search mode=ast rejects malformed query", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("bad_query");
    write_file(tmp / "a.cpp", "int x = 0;\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;
        // Unmatched paren -- syntax error.
        auto result = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"},
                 {"query", "(call_expression"}}}, ws);
        REQUIRE_FALSE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("invalid Tree-sitter query"));
    }
    cleanup(tmp);
}

// -- Predicate-based filtering ----------------------------------------------

TEST_CASE("search ast finds specific function calls via #eq? predicate", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("malloc");
    write_file(tmp / "alloc.cpp",
        "#include <cstdlib>\n"
        "void* a() { return malloc(16); }\n"
        "void  b() { free(a()); }\n"
        "void* c() { return malloc(32); }\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;
        auto result = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"},
                 {"query",
                  "(call_expression function: (identifier) @fn "
                  "(#eq? @fn \"malloc\"))"},
                 {"capture", "fn"}}}, ws);
        REQUIRE(result.success);
        // Both malloc sites, neither free.
        REQUIRE_THAT(result.content, ContainsSubstring("alloc.cpp:2"));
        REQUIRE_THAT(result.content, ContainsSubstring("alloc.cpp:4"));
        REQUIRE_THAT(result.content, ContainsSubstring("2 matches"));
        REQUIRE_THAT(result.content, !ContainsSubstring("alloc.cpp:3"));
    }
    cleanup(tmp);
}

// -- Capture filter ---------------------------------------------------------

TEST_CASE("search ast capture parameter narrows reported captures", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("capture_filter");
    write_file(tmp / "a.cpp",
        "void g() { foo(); bar(); }\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;

        // Without capture filter both @fn and @call are reported per match.
        auto all_caps = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"},
                 {"query", "(call_expression function: (_) @fn) @call"}}}, ws);
        REQUIRE(all_caps.success);
        REQUIRE_THAT(all_caps.content, ContainsSubstring("@fn"));
        REQUIRE_THAT(all_caps.content, ContainsSubstring("@call"));

        // With capture=fn, only @fn rows appear.
        auto only_fn = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"},
                 {"query", "(call_expression function: (_) @fn) @call"},
                 {"capture", "fn"}}}, ws);
        REQUIRE(only_fn.success);
        REQUIRE_THAT(only_fn.content, ContainsSubstring("@fn"));
        REQUIRE_THAT(only_fn.content, !ContainsSubstring("@call"));
    }
    cleanup(tmp);
}

// -- Language scoping -------------------------------------------------------

TEST_CASE("search ast only visits files of the requested language", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("lang_scope");
    write_file(tmp / "a.cpp", "void f() { foo(); }\n");
    write_file(tmp / "b.py",  "def g():\n    foo()\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;

        // C++ query against C++ files only; the .py file is not parsed.
        auto cpp = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"},
                 {"query", "(call_expression function: (identifier) @fn)"}}}, ws);
        REQUIRE(cpp.success);
        REQUIRE_THAT(cpp.content, ContainsSubstring("a.cpp"));
        REQUIRE_THAT(cpp.content, !ContainsSubstring("b.py"));

        auto py = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "python"},
                 {"query", "(call function: (identifier) @fn)"}}}, ws);
        REQUIRE(py.success);
        REQUIRE_THAT(py.content, ContainsSubstring("b.py"));
        REQUIRE_THAT(py.content, !ContainsSubstring("a.cpp"));
    }
    cleanup(tmp);
}

// -- path_glob filter -------------------------------------------------------

TEST_CASE("search ast respects path_glob", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("glob");
    write_file(tmp / "keep" / "a.cpp", "void f() { foo(); }\n");
    write_file(tmp / "skip" / "b.cpp", "void g() { foo(); }\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;
        auto result = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"},
                 {"query", "(call_expression function: (identifier) @fn)"},
                 {"path_glob", "keep/**"}}}, ws);
        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("keep/a.cpp"));
        REQUIRE_THAT(result.content, !ContainsSubstring("skip/b.cpp"));
    }
    cleanup(tmp);
}

// -- max_results cap --------------------------------------------------------

TEST_CASE("search ast respects max_results", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("cap");
    std::string body;
    for (int i = 0; i < 30; ++i) body += "    foo();\n";
    write_file(tmp / "many.cpp", "void f() {\n" + body + "}\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;
        auto result = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"},
                 {"query", "(call_expression function: (identifier) @fn)"},
                 {"capture", "fn"},
                 {"max_results", 5}}}, ws);
        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("5 matches"));
        REQUIRE_THAT(result.content, ContainsSubstring("truncated"));
    }
    cleanup(tmp);
}

// -- Snippet output ---------------------------------------------------------

TEST_CASE("search ast emits per-match snippet with surrounding line", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("snippet");
    write_file(tmp / "ctx.cpp",
        "// header line\n"
        "void f() {\n"
        "    int x = malloc(8);\n"
        "    return;\n"
        "}\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;
        auto result = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"},
                 {"query",
                  "(call_expression function: (identifier) @fn "
                  "(#eq? @fn \"malloc\"))"},
                 {"capture", "fn"}}}, ws);
        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("ctx.cpp:3"));
        // Surrounding context lines should appear.
        REQUIRE_THAT(result.content, ContainsSubstring("void f()"));
        REQUIRE_THAT(result.content, ContainsSubstring("return;"));
    }
    cleanup(tmp);
}

// -- WS1-style smoke: classes deriving from a base --------------------------

TEST_CASE("search ast finds classes inheriting from a base type", "[s4.m][search][ast]")
{
    auto tmp = make_test_dir("derived");
    write_file(tmp / "tools.hpp",
        "class ITool {};\n"
        "class FooTool : public ITool {};\n"
        "class BarHelper {};\n"
        "class BazTool : public ITool {};\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchAstTool tool;
        auto result = tool.execute(
            locus::ToolCall{"a", "search_ast",
                {{"language", "cpp"},
                 {"query",
                  "(class_specifier name: (type_identifier) @name "
                  "(base_class_clause (type_identifier) @base "
                  "(#eq? @base \"ITool\")))"},
                 {"capture", "name"}}}, ws);
        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("@name  FooTool"));
        REQUIRE_THAT(result.content, ContainsSubstring("@name  BazTool"));
        // BarHelper may appear in a neighbouring-line snippet but must not
        // be reported as a captured @name.
        REQUIRE_THAT(result.content, !ContainsSubstring("@name  BarHelper"));
        REQUIRE_THAT(result.content, ContainsSubstring("2 matches"));
    }
    cleanup(tmp);
}

// -- Schema -----------------------------------------------------------------

TEST_CASE("search tool advertises ast mode in its schema", "[s4.m][search][ast]")
{
    locus::SearchTool tool;
    auto desc = tool.description();
    REQUIRE_THAT(desc, ContainsSubstring("ast"));

    bool has_capture = false, has_lang = false;
    for (auto& p : tool.params()) {
        if (p.name == "capture")  has_capture = true;
        if (p.name == "language") has_lang    = true;
    }
    REQUIRE(has_capture);
    REQUIRE(has_lang);
}
