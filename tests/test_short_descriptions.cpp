// S6.17 Task I -- verify each built-in tool's short_description() names every
// required arg and (where applicable) every enum-valued arg's allowed values.
//
// Under lazy_tool_manifest, the model can't see arg names without calling
// describe_tool. Cramming "tool_name(arg1, arg2=default)" into the one-liner
// is the cheapest signal that closes the schema-discovery gap.

#include "tools/tools.h"
#include "tools/tool_registry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;

// Build the registry, look up `name`, and assert each entry in `must_contain`
// is a substring of short_description().
static void assert_cues(const char* name,
                        std::initializer_list<const char*> must_contain)
{
    locus::ToolRegistry reg;
    locus::register_builtin_tools(reg);
    auto* t = reg.find(name);
    REQUIRE(t != nullptr);
    auto sd = t->short_description();
    INFO("tool=" << name << " short_description=" << sd);
    for (const char* needle : must_contain)
        REQUIRE_THAT(sd, ContainsSubstring(needle));
}

TEST_CASE("short_description: read_file names path+offset+length",
          "[s6.17][short_description]")
{
    assert_cues("read_file", {"path", "offset", "length"});
}

TEST_CASE("short_description: write_file names path+content",
          "[s6.17][short_description]")
{
    assert_cues("write_file", {"path", "content"});
}

TEST_CASE("short_description: edit_file names path+edits",
          "[s6.17][short_description]")
{
    assert_cues("edit_file", {"path", "edits", "old_string", "new_string"});
}

TEST_CASE("short_description: delete_file names path",
          "[s6.17][short_description]")
{
    assert_cues("delete_file", {"path"});
}

// S6.17 Task G -- the unified `search` tool was split into per-mode tools.
// Each per-mode tool now names its own canonical args inline.
TEST_CASE("short_description: search_text names query",
          "[s6.17][short_description]")
{
    assert_cues("search_text", {"query"});
}

TEST_CASE("short_description: search_regex names query+max_results",
          "[s6.17][short_description]")
{
    assert_cues("search_regex", {"query", "max_results"});
}

TEST_CASE("short_description: search_symbols names name arg (not query)",
          "[s6.17][short_description]")
{
    assert_cues("search_symbols", {"name"});
}

TEST_CASE("short_description: search_semantic names query",
          "[s6.17][short_description]")
{
    assert_cues("search_semantic", {"query"});
}

// search_hybrid retired in ADR-0009 -- no short_description to assert.

TEST_CASE("short_description: search_ast names language+query+capture",
          "[s6.17][short_description]")
{
    assert_cues("search_ast", {"language", "query", "capture"});
}

TEST_CASE("short_description: list_directory names path+depth",
          "[s6.17][short_description]")
{
    assert_cues("list_directory", {"path", "depth"});
}

TEST_CASE("short_description: get_file_outline names path",
          "[s6.17][short_description]")
{
    assert_cues("get_file_outline", {"path"});
}

TEST_CASE("short_description: run_command names command",
          "[s6.17][short_description]")
{
    assert_cues("run_command", {"command"});
}

TEST_CASE("short_description: ask_user names question",
          "[s6.17][short_description]")
{
    assert_cues("ask_user", {"question"});
}

TEST_CASE("short_description: describe_tool names name arg",
          "[s6.17][short_description]")
{
    assert_cues("describe_tool", {"name"});
}
