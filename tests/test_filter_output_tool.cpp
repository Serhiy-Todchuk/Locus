#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "tools/filter_output_tool.h"
#include "support/fake_workspace_services.h"

using Catch::Matchers::ContainsSubstring;

namespace {

locus::test::FakeWorkspaceServices make_ws()
{
    return locus::test::FakeWorkspaceServices(std::filesystem::temp_directory_path());
}

locus::ToolCall make_call(nlohmann::json args)
{
    return {"c", "filter_output", std::move(args)};
}

const std::string k_log =
    "build started\n"
    "[ 10%] Compiling foo.cpp\n"
    "[ 20%] Compiling bar.cpp\n"
    "bar.cpp(42): error C2660: too many arguments\n"
    "[ 30%] Compiling baz.cpp\n"
    "baz.cpp(1): warning C4101: unreferenced local variable\n"
    "baz.cpp(7): error C2065: undeclared identifier\n"
    "[ 40%] Linking\n"
    "fatal error LNK1120: 2 unresolved externals\n"
    "build failed\n";

} // namespace

TEST_CASE("FilterOutputTool: regex over a build log", "[s5.z][filter_output]")
{
    auto ws = make_ws();
    locus::FilterOutputTool tool;

    auto r = tool.execute(make_call({
        {"text",    k_log},
        {"pattern", "error"},
        {"mode",    "regex"},
    }), ws);

    REQUIRE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("3 of 10 lines matched"));
    REQUIRE_THAT(r.content, ContainsSubstring("error C2660"));
    REQUIRE_THAT(r.content, ContainsSubstring("error C2065"));
    REQUIRE_THAT(r.content, ContainsSubstring("LNK1120"));
    // The "warning C4101" line has the substring "ar" in it but does NOT
    // match "error" -- guard against accidental over-broadening.
    REQUIRE_THAT(r.content, !ContainsSubstring("warning C4101"));
}

TEST_CASE("FilterOutputTool: substring mode is literal, ignores regex meta",
          "[s5.z][filter_output]")
{
    auto ws = make_ws();
    locus::FilterOutputTool tool;

    auto r = tool.execute(make_call({
        {"text",    "line one\nfile.cpp(7)\nline three\n"},
        {"pattern", "."},     // matches any char in regex, only literal in substring
        {"mode",    "substring"},
    }), ws);

    REQUIRE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("1 of 3 lines matched"));
    REQUIRE_THAT(r.content, ContainsSubstring("file.cpp(7)"));
}

TEST_CASE("FilterOutputTool: case_sensitive defaults to false",
          "[s5.z][filter_output]")
{
    auto ws = make_ws();
    locus::FilterOutputTool tool;

    auto r = tool.execute(make_call({
        {"text",    "Foo\nBAR\nbaz\n"},
        {"pattern", "FOO"},
    }), ws);
    REQUIRE_THAT(r.content, ContainsSubstring("1 of 3 lines matched"));
    REQUIRE_THAT(r.content, ContainsSubstring("Foo"));

    auto rc = tool.execute(make_call({
        {"text",           "Foo\nBAR\nbaz\n"},
        {"pattern",        "FOO"},
        {"case_sensitive", true},
    }), ws);
    REQUIRE_THAT(rc.content, ContainsSubstring("0 of 3 lines matched"));
}

TEST_CASE("FilterOutputTool: before/after context lines", "[s5.z][filter_output]")
{
    auto ws = make_ws();
    locus::FilterOutputTool tool;

    auto r = tool.execute(make_call({
        {"text",    "a\nb\nc\nERROR x\nd\ne\nf\n"},
        {"pattern", "ERROR"},
        {"before",  1},
        {"after",   2},
    }), ws);

    REQUIRE(r.success);
    // Line numbers: c=3 (context), ERROR x=4 (match), d=5 (context), e=6 (context).
    REQUIRE_THAT(r.content, ContainsSubstring("3-c"));
    REQUIRE_THAT(r.content, ContainsSubstring("4:ERROR x"));
    REQUIRE_THAT(r.content, ContainsSubstring("5-d"));
    REQUIRE_THAT(r.content, ContainsSubstring("6-e"));
    // f (line 7) is outside the after=2 window.
    REQUIRE_THAT(r.content, !ContainsSubstring("7-f"));
}

TEST_CASE("FilterOutputTool: max_matches caps response + summary reports total",
          "[s5.z][filter_output]")
{
    auto ws = make_ws();
    locus::FilterOutputTool tool;

    std::string text;
    for (int i = 0; i < 100; ++i) text += "hit line " + std::to_string(i) + "\n";

    auto r = tool.execute(make_call({
        {"text",        text},
        {"pattern",     "hit"},
        {"max_matches", 5},
    }), ws);

    REQUIRE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("100 of 100 lines matched"));
    REQUIRE_THAT(r.content, ContainsSubstring("(capped at 5)"));
    REQUIRE_THAT(r.content, ContainsSubstring("hit line 0"));
    REQUIRE_THAT(r.content, ContainsSubstring("hit line 4"));
    REQUIRE_THAT(r.content, !ContainsSubstring("hit line 5"));
}

TEST_CASE("FilterOutputTool: invert returns non-matching lines",
          "[s5.z][filter_output]")
{
    auto ws = make_ws();
    locus::FilterOutputTool tool;

    auto r = tool.execute(make_call({
        {"text",    "ok\nFAIL\nok\nok\n"},
        {"pattern", "FAIL"},
        {"invert",  true},
    }), ws);

    REQUIRE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("3 of 4 lines matched"));
    REQUIRE_THAT(r.content, !ContainsSubstring("FAIL"));
}

TEST_CASE("FilterOutputTool: invalid regex returns a clean error",
          "[s5.z][filter_output]")
{
    auto ws = make_ws();
    locus::FilterOutputTool tool;

    auto r = tool.execute(make_call({
        {"text",    "x\n"},
        {"pattern", "("},      // unbalanced paren
    }), ws);

    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("invalid regex"));
}

TEST_CASE("FilterOutputTool: missing required params fail with a message",
          "[s5.z][filter_output]")
{
    auto ws = make_ws();
    locus::FilterOutputTool tool;

    auto r1 = tool.execute(make_call({{"pattern", "x"}}), ws);
    REQUIRE_FALSE(r1.success);
    REQUIRE_THAT(r1.content, ContainsSubstring("'text' parameter is required"));

    auto r2 = tool.execute(make_call({{"text", "x"}}), ws);
    REQUIRE_FALSE(r2.success);
    REQUIRE_THAT(r2.content, ContainsSubstring("'pattern' parameter is required"));
}

TEST_CASE("FilterOutputTool: CRLF line endings collapse correctly",
          "[s5.z][filter_output]")
{
    auto ws = make_ws();
    locus::FilterOutputTool tool;

    auto r = tool.execute(make_call({
        {"text",    "alpha\r\nbeta\r\ngamma\r\n"},
        {"pattern", "beta"},
    }), ws);
    REQUIRE_THAT(r.content, ContainsSubstring("1 of 3 lines matched"));
    REQUIRE_THAT(r.content, ContainsSubstring("2:beta"));
}
