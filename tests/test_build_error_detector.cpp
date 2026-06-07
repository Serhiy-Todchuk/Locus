// S6.20 -- unit tests for the "stuck on the same build error" signature
// extractor. The detector that consumes it (AgentTurnRunner::check_stuck_error)
// is integration-shaped (needs a live turn); these tests pin the pure function
// that decides whether two build outputs represent the SAME failure.

#include "agent/build_error_detector.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using locus::extract_error_signature;

TEST_CASE("extract_error_signature: empty / no-error output yields empty",
          "[s6.20][build_error_detector]")
{
    REQUIRE(extract_error_signature("").empty());
    REQUIRE(extract_error_signature("Build succeeded.\n0 errors\n").empty());
    REQUIRE(extract_error_signature(
        "-- Build files have been written to: D:/proj/build\n").empty());
    // A warning is NOT a "stuck" signal -- only hard errors count.
    REQUIRE(extract_error_signature(
        "foo.cpp(12): warning C4244: conversion possible loss of data\n").empty());
}

TEST_CASE("extract_error_signature: MSVC error is captured without file/line noise",
          "[s6.20][build_error_detector]")
{
    std::string out =
        "D:\\proj\\src\\main.cpp(491,67): error C2102: '&' requires l-value "
        "[D:\\proj\\build\\app.vcxproj]\n";
    std::string sig = extract_error_signature(out);
    REQUIRE_FALSE(sig.empty());
    REQUIRE_THAT(sig, ContainsSubstring("C2102"));
    REQUIRE_THAT(sig, ContainsSubstring("requires l-value"));
    // The file path + vcxproj annotation must NOT be in the signature.
    REQUIRE_THAT(sig, !ContainsSubstring("main.cpp"));
    REQUIRE_THAT(sig, !ContainsSubstring("vcxproj"));
}

TEST_CASE("extract_error_signature: same error at different lines -> same signature",
          "[s6.20][build_error_detector]")
{
    // The whole point: the model rewrites the file, the error moves to a new
    // line/column, but it's the SAME failure -> identical signature.
    std::string a = "D:\\p\\main.cpp(491,67): error C2102: '&' requires l-value\n";
    std::string b = "D:\\p\\main.cpp(625,12): error C2102: '&' requires l-value\n";
    REQUIRE(extract_error_signature(a) == extract_error_signature(b));
}

TEST_CASE("extract_error_signature: reordered diagnostics -> same signature",
          "[s6.20][build_error_detector]")
{
    std::string a =
        "main.cpp(10): error C2065: 'foo' undeclared\n"
        "main.cpp(20): error C3861: 'bar' identifier not found\n";
    std::string b =   // same two errors, emitted in the other order
        "main.cpp(33): error C3861: 'bar' identifier not found\n"
        "main.cpp(44): error C2065: 'foo' undeclared\n";
    REQUIRE(extract_error_signature(a) == extract_error_signature(b));
}

TEST_CASE("extract_error_signature: different errors -> different signatures",
          "[s6.20][build_error_detector]")
{
    std::string a = "main.cpp(10): error C2102: '&' requires l-value\n";
    std::string b = "main.cpp(10): error C2065: 'foo' undeclared identifier\n";
    REQUIRE(extract_error_signature(a) != extract_error_signature(b));
}

TEST_CASE("extract_error_signature: GCC/Clang style is recognized",
          "[s6.20][build_error_detector]")
{
    std::string out =
        "src/main.cpp:42:5: error: expected ';' before '}' token\n";
    std::string sig = extract_error_signature(out);
    REQUIRE_FALSE(sig.empty());
    REQUIRE_THAT(sig, ContainsSubstring("expected ';'"));
    REQUIRE_THAT(sig, !ContainsSubstring("main.cpp"));
}

TEST_CASE("extract_error_signature: MSVC linker fatal error is captured",
          "[s6.20][build_error_detector]")
{
    std::string out =
        "app.obj : error LNK2019: unresolved external symbol foo\n"
        "D:\\p\\app.exe : fatal error LNK1120: 1 unresolved externals\n";
    std::string sig = extract_error_signature(out);
    REQUIRE_THAT(sig, ContainsSubstring("LNK2019"));
    REQUIRE_THAT(sig, ContainsSubstring("LNK1120"));
}

TEST_CASE("extract_error_signature: line-count drift in the same LNK error is stable",
          "[s6.20][build_error_detector]")
{
    // "1 unresolved externals" vs "3 unresolved externals" -- the count is
    // numeric noise; both are the same stuck-linker signature.
    std::string a = "app.exe : fatal error LNK1120: 1 unresolved externals\n";
    std::string b = "app.exe : fatal error LNK1120: 3 unresolved externals\n";
    REQUIRE(extract_error_signature(a) == extract_error_signature(b));
}
