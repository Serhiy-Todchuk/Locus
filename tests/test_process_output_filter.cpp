#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "tools/process_output_filter.h"

using Catch::Matchers::ContainsSubstring;

namespace {

std::string build_lines(int n, const char* prefix = "line")
{
    std::string out;
    out.reserve(static_cast<std::size_t>(n) * 10);
    for (int i = 1; i <= n; ++i) {
        out += prefix;
        out += " ";
        out += std::to_string(i);
        out += "\n";
    }
    return out;
}

int count_lines(const std::string& s)
{
    int n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (!s.empty() && s.back() != '\n') ++n;
    return n;
}

} // namespace

TEST_CASE("process_output_filter: default head_tail short-circuits when small",
          "[output_filter]")
{
    auto text = build_lines(20);
    locus::OutputFilterSpec spec;  // empty -> default head_tail
    auto out = locus::apply_output_filter(text, spec, /*default_lines=*/50);
    // 20 lines <= 2*50, so output should be unchanged.
    REQUIRE(out == text);
}

TEST_CASE("process_output_filter: head_tail trims middle when large",
          "[output_filter]")
{
    auto text = build_lines(500);
    locus::OutputFilterSpec spec;  // default
    auto out = locus::apply_output_filter(text, spec, /*default_lines=*/50);
    REQUIRE_THAT(out, ContainsSubstring("line 1\n"));
    REQUIRE_THAT(out, ContainsSubstring("line 50\n"));
    REQUIRE_THAT(out, ContainsSubstring("line 451"));
    REQUIRE_THAT(out, ContainsSubstring("line 500"));
    REQUIRE_THAT(out, ContainsSubstring("400 lines elided"));
    REQUIRE_THAT(out, ContainsSubstring(".locus/locus.log"));
    // 51..450 must NOT be present.
    REQUIRE(out.find("line 250\n") == std::string::npos);
}

TEST_CASE("process_output_filter: head mode keeps first N", "[output_filter]")
{
    auto text = build_lines(200);
    locus::OutputFilterSpec spec;
    spec.mode = "head";
    spec.lines = 10;
    auto out = locus::apply_output_filter(text, spec, /*default_lines=*/50);
    REQUIRE_THAT(out, ContainsSubstring("line 1\n"));
    REQUIRE_THAT(out, ContainsSubstring("line 10\n"));
    REQUIRE(out.find("line 11\n") == std::string::npos);
    REQUIRE_THAT(out, ContainsSubstring("190 lines elided"));
}

TEST_CASE("process_output_filter: tail mode keeps last N", "[output_filter]")
{
    auto text = build_lines(200);
    locus::OutputFilterSpec spec;
    spec.mode = "tail";
    spec.lines = 10;
    auto out = locus::apply_output_filter(text, spec, /*default_lines=*/50);
    REQUIRE_THAT(out, ContainsSubstring("line 191\n"));
    REQUIRE_THAT(out, ContainsSubstring("line 200"));
    REQUIRE(out.find("line 190\n") == std::string::npos);
    REQUIRE_THAT(out, ContainsSubstring("190 lines elided"));
}

TEST_CASE("process_output_filter: regex mode with context", "[output_filter]")
{
    std::string text =
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

    locus::OutputFilterSpec spec;
    spec.mode = "regex";
    spec.pattern = "error|warning";
    spec.lines = 10;
    spec.context = 1;
    auto out = locus::apply_output_filter(text, spec, /*default_lines=*/50);
    REQUIRE_THAT(out, ContainsSubstring("error C2660"));
    REQUIRE_THAT(out, ContainsSubstring("warning C4101"));
    REQUIRE_THAT(out, ContainsSubstring("error C2065"));
    REQUIRE_THAT(out, ContainsSubstring("LNK1120"));
    // Context line above first error.
    REQUIRE_THAT(out, ContainsSubstring("Compiling bar.cpp"));
    // Build-started line is NOT in any match window.
    REQUIRE(out.find("build started") == std::string::npos);
}

TEST_CASE("process_output_filter: substring mode is literal", "[output_filter]")
{
    std::string text =
        "foo.bar\n"
        "fooXbar\n"
        "no match here\n"
        "another foo.bar line\n";

    locus::OutputFilterSpec spec;
    spec.mode = "substring";
    spec.pattern = "foo.bar";  // '.' is literal, not regex
    spec.lines = 5;
    auto out = locus::apply_output_filter(text, spec, /*default_lines=*/50);
    REQUIRE_THAT(out, ContainsSubstring("foo.bar"));
    REQUIRE(out.find("fooXbar") == std::string::npos);
}

TEST_CASE("process_output_filter: empty input returns empty", "[output_filter]")
{
    locus::OutputFilterSpec spec;
    auto out = locus::apply_output_filter("", spec, /*default_lines=*/50);
    REQUIRE(out.empty());
}

TEST_CASE("process_output_filter: default_lines=0 disables smart-truncate",
          "[output_filter]")
{
    auto text = build_lines(500);
    locus::OutputFilterSpec spec;  // mode=""
    auto out = locus::apply_output_filter(text, spec, /*default_lines=*/0);
    REQUIRE(out == text);
}

TEST_CASE("process_output_filter: bad regex surfaces as marker", "[output_filter]")
{
    auto text = build_lines(20);
    locus::OutputFilterSpec spec;
    spec.mode = "regex";
    spec.pattern = "[unterminated";
    spec.lines = 5;
    auto out = locus::apply_output_filter(text, spec, /*default_lines=*/50);
    REQUIRE_THAT(out, ContainsSubstring("invalid regex"));
}

TEST_CASE("process_output_filter: parse_output_filter_args validates", "[output_filter]")
{
    nlohmann::json args;
    locus::OutputFilterSpec spec;
    std::string err;

    SECTION("empty -> defaults") {
        REQUIRE(locus::parse_output_filter_args(args, spec, err));
        REQUIRE(spec.mode.empty());
        REQUIRE(spec.lines == 0);
        REQUIRE(spec.context == 0);
    }
    SECTION("good values") {
        args["output_filter_mode"]    = "regex";
        args["output_filter_pattern"] = "err";
        args["output_filter_lines"]   = 20;
        args["output_filter_context"] = 2;
        REQUIRE(locus::parse_output_filter_args(args, spec, err));
        REQUIRE(spec.mode == "regex");
        REQUIRE(spec.pattern == "err");
        REQUIRE(spec.lines == 20);
        REQUIRE(spec.context == 2);
    }
    SECTION("unknown mode rejected") {
        args["output_filter_mode"] = "magic";
        REQUIRE_FALSE(locus::parse_output_filter_args(args, spec, err));
        REQUIRE_THAT(err, ContainsSubstring("output_filter_mode"));
    }
    SECTION("context clamped to 10") {
        args["output_filter_context"] = 50;
        REQUIRE(locus::parse_output_filter_args(args, spec, err));
        REQUIRE(spec.context == 10);
    }
    SECTION("negative lines clamped to 0") {
        args["output_filter_lines"] = -3;
        REQUIRE(locus::parse_output_filter_args(args, spec, err));
        REQUIRE(spec.lines == 0);
    }
}
