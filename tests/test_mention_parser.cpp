// S4.V Task 3 -- @-mention parser. Verified independently of the GUI: the
// parser is a pure function over UTF-8 text.

#include <catch2/catch_test_macros.hpp>

#include "agent/mention_parser.h"

using locus::parse_mentions;

TEST_CASE("parse_mentions picks up a single workspace-relative path",
          "[s4.v][mentions]")
{
    auto m = parse_mentions("please read @src/agent/agent_core.cpp for context");
    REQUIRE(m.size() == 1);
    REQUIRE(m[0].path == "src/agent/agent_core.cpp");
    REQUIRE(m[0].start == 12);  // position of '@'
    REQUIRE(m[0].end == 12 + 1 + m[0].path.size());
}

TEST_CASE("parse_mentions returns multiple in source order", "[s4.v][mentions]")
{
    auto m = parse_mentions("diff @a.txt vs @b.txt please");
    REQUIRE(m.size() == 2);
    REQUIRE(m[0].path == "a.txt");
    REQUIRE(m[1].path == "b.txt");
    REQUIRE(m[0].start < m[1].start);
}

TEST_CASE("parse_mentions ignores email-shaped @", "[s4.v][mentions]")
{
    auto m = parse_mentions("ping me at user@example.com about this");
    REQUIRE(m.empty());

    auto m2 = parse_mentions("multi.part.name@corp.local works too");
    REQUIRE(m2.empty());
}

TEST_CASE("parse_mentions accepts @ after common punctuation", "[s4.v][mentions]")
{
    auto a = parse_mentions("(@src/foo.cpp)");
    REQUIRE(a.size() == 1);
    REQUIRE(a[0].path == "src/foo.cpp");

    auto b = parse_mentions("[@src/foo.cpp]");
    REQUIRE(b.size() == 1);
    REQUIRE(b[0].path == "src/foo.cpp");

    auto c = parse_mentions(",@src/foo.cpp");
    REQUIRE(c.size() == 1);
    REQUIRE(c[0].path == "src/foo.cpp");
}

TEST_CASE("parse_mentions strips trailing dots and stops at whitespace",
          "[s4.v][mentions]")
{
    auto a = parse_mentions("see @src/agent/agent_core.cpp.");
    REQUIRE(a.size() == 1);
    REQUIRE(a[0].path == "src/agent/agent_core.cpp");

    auto b = parse_mentions("see @src/agent/agent_core.cpp, please");
    REQUIRE(b.size() == 1);
    REQUIRE(b[0].path == "src/agent/agent_core.cpp");

    auto c = parse_mentions("@a/b/c.h\n@x.h");
    REQUIRE(c.size() == 2);
    REQUIRE(c[0].path == "a/b/c.h");
    REQUIRE(c[1].path == "x.h");
}

TEST_CASE("parse_mentions handles Windows backslash paths", "[s4.v][mentions]")
{
    auto m = parse_mentions("look at @src\\agent\\agent_core.cpp now");
    REQUIRE(m.size() == 1);
    REQUIRE(m[0].path == "src\\agent\\agent_core.cpp");
}

TEST_CASE("parse_mentions rejects bare @ with no path", "[s4.v][mentions]")
{
    REQUIRE(parse_mentions("hey @ there").empty());
    REQUIRE(parse_mentions("@").empty());
    REQUIRE(parse_mentions("@  ").empty());
}

TEST_CASE("parse_mentions handles @ at start of input", "[s4.v][mentions]")
{
    auto m = parse_mentions("@src/main.cpp explain this");
    REQUIRE(m.size() == 1);
    REQUIRE(m[0].start == 0);
    REQUIRE(m[0].path == "src/main.cpp");
}
