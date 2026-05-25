// S6.10 Task G -- anti-truncation detector unit tests.
//
// Pure-function tests over `detect_truncation_phrase`. Integration with
// WriteFileTool / EditFileTool is exercised in test_tools.cpp where the
// dispatcher gate, ReadTracker preconditions, and atomic-write paths
// already have coverage.

#include "tools/truncation_detector.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using locus::tools::detect_truncation_phrase;

TEST_CASE("truncation_detector: empty input returns nullopt",
          "[s6.10][truncation]")
{
    REQUIRE_FALSE(detect_truncation_phrase("").has_value());
}

TEST_CASE("truncation_detector: real C++ file with no markers returns nullopt",
          "[s6.10][truncation]")
{
    std::string body =
        "#include <vector>\n"
        "int main() {\n"
        "    std::vector<int> v{1, 2, 3};\n"
        "    return v.size();\n"
        "}\n";
    REQUIRE_FALSE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: bare ... does NOT trigger (Python/JS/C++ idiom)",
          "[s6.10][truncation]")
{
    // Python Ellipsis literal, JS spread, C++ varargs all use bare `...`.
    // The detector must not flag any of these.
    REQUIRE_FALSE(detect_truncation_phrase("def x(): ...\n").has_value());
    REQUIRE_FALSE(detect_truncation_phrase("const a = [...b];\n").has_value());
    REQUIRE_FALSE(detect_truncation_phrase("void f(...);\n").has_value());
}

TEST_CASE("truncation_detector: C++ // rest of the code at end",
          "[s6.10][truncation]")
{
    std::string body =
        "void foo() {\n"
        "    bar();\n"
        "    // rest of the code\n"
        "}\n";
    auto r = detect_truncation_phrase(body);
    REQUIRE(r.has_value());
    REQUIRE(r->find("rest of the code") != std::string::npos);
}

TEST_CASE("truncation_detector: // rest of the implementation",
          "[s6.10][truncation]")
{
    std::string body =
        "int compute() {\n"
        "    // rest of the implementation\n"
        "    return 0;\n"
        "}\n";
    REQUIRE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: // ... existing code ...",
          "[s6.10][truncation]")
{
    std::string body =
        "void f() {\n"
        "    init();\n"
        "    // ... existing code ...\n"
        "}\n";
    auto r = detect_truncation_phrase(body);
    REQUIRE(r.has_value());
    REQUIRE(r->find("existing code") != std::string::npos);
}

TEST_CASE("truncation_detector: // (continued) marker",
          "[s6.10][truncation]")
{
    std::string body = "doStuff();\n// (continued)\n";
    REQUIRE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: Python # rest of the code",
          "[s6.10][truncation]")
{
    std::string body =
        "def foo():\n"
        "    bar()\n"
        "    # rest of the code\n";
    REQUIRE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: Python # rest of the implementation",
          "[s6.10][truncation]")
{
    std::string body =
        "def foo():\n"
        "    # rest of the implementation\n";
    REQUIRE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: /* ... rest ... */ block",
          "[s6.10][truncation]")
{
    std::string body =
        "int main() {\n"
        "    work();\n"
        "    /* ... rest ... */\n"
        "}\n";
    REQUIRE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: HTML <!-- rest of the code -->",
          "[s6.10][truncation]")
{
    std::string body =
        "<body>\n"
        "  <header>...</header>\n"
        "  <!-- rest of the code -->\n"
        "</body>\n";
    REQUIRE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: case-insensitive matching",
          "[s6.10][truncation]")
{
    std::string body =
        "void f() {\n"
        "    // REST OF THE CODE\n"
        "}\n";
    REQUIRE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: trailing whitespace is trimmed",
          "[s6.10][truncation]")
{
    // The detector should fire even if the marker has trailing newlines,
    // spaces, or a stray closing brace after it.
    std::string body =
        "void f() {\n"
        "    // rest of the code\n"
        "}\n"
        "\n\n   \n";
    REQUIRE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: TODO marker only fires near EOF",
          "[s6.10][truncation]")
{
    // Mid-file TODO is legit code, must not trigger.
    std::string mid =
        "void f() {\n"
        "    // TODO: implement\n"
        "    return baz();\n"
        "}\n"
        "void g() {\n"
        "    really_long_real_body();\n"
        "    more_real_body();\n"
        "    even_more();\n"
        "    final_call();\n"
        "    return;\n"
        "}\n";
    REQUIRE_FALSE(detect_truncation_phrase(mid).has_value());

    // EOF TODO is the truncation signal.
    std::string eof =
        "void f() {\n"
        "    // TODO: implement\n";
    REQUIRE(detect_truncation_phrase(eof).has_value());
}

TEST_CASE("truncation_detector: marker more than 200 chars from end is ignored",
          "[s6.10][truncation]")
{
    // The detector slices the last 200 chars -- a marker earlier in the file
    // (followed by lots of real content) is not a truncation signal.
    std::string body = "void f() {\n    // rest of the code (in a doc-string)\n";
    body += "    return ";
    // ~250 chars of legitimate-looking content after the marker.
    for (int i = 0; i < 30; ++i) body += "real_function_call(); ";
    body += ";\n}\n";
    REQUIRE_FALSE(detect_truncation_phrase(body).has_value());
}

TEST_CASE("truncation_detector: marker preceded by real content within window",
          "[s6.10][truncation]")
{
    // Marker within the last 200 chars still trips, even with some trailing
    // closing braces.
    std::string body = "void f() {\n";
    body += "    real_call();\n";
    body += "    // rest of the code\n";
    body += "    return 0;\n";
    body += "}\n";
    REQUIRE(detect_truncation_phrase(body).has_value());
}
