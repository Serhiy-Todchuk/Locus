// S6.17 Task D -- per-tool unknown-key rejection. TestLocalVibe2's Pass 5
// burned ~12 rounds on tool-schema discovery because tools silently fell
// through to defaults on alias arg names (lines / type / cmd). These tests
// pin the new explicit-error behaviour.

#include "support/fake_workspace_services.h"

#include "tools/file_tools.h"
#include "tools/index_tools.h"
#include "tools/process_tools.h"
#include "tools/search_tools.h"
#include "tools/shared.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path make_tmp()
{
    auto root = fs::temp_directory_path() /
                ("locus_arg_val_" + std::to_string(::time(nullptr)));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& root, const std::string& rel,
                const std::string& body)
{
    fs::path full = root / rel;
    fs::create_directories(full.parent_path());
    std::ofstream(full, std::ios::binary) << body;
}

} // namespace

TEST_CASE("read_file rejects 'lines' alias with offset+length suggestion",
          "[s6.17][tool_arg_validation]")
{
    auto tmp = make_tmp();
    write_file(tmp, "f.txt", "a\nb\nc\n");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::ReadFileTool tool;
    locus::ToolCall call{"c1", "read_file",
        {{"path", "f.txt"}, {"lines", "1-2"}}};
    auto r = tool.execute(call, ws);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("unrecognized argument 'lines'"));
    REQUIRE_THAT(r.content, ContainsSubstring("offset"));
    REQUIRE_THAT(r.content, ContainsSubstring("length"));

    fs::remove_all(tmp);
}

TEST_CASE("read_file accepts canonical offset+length without rejection",
          "[s6.17][tool_arg_validation]")
{
    auto tmp = make_tmp();
    write_file(tmp, "f.txt", "alpha\nbeta\ngamma\n");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::ReadFileTool tool;
    locus::ToolCall call{"c1", "read_file",
        {{"path", "f.txt"}, {"offset", 2}, {"length", 1}}};
    auto r = tool.execute(call, ws);
    REQUIRE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("beta"));
    // S6.17 Task D -- header carries applied args.
    REQUIRE_THAT(r.content, ContainsSubstring("offset=2"));
    REQUIRE_THAT(r.content, ContainsSubstring("length=1"));

    fs::remove_all(tmp);
}

// S6.17 Task G -- the unified `search` tool was split into per-mode tools.
// A model that sends `type: "regex"` to `search_text` (looking for the
// retired discriminator) gets an explicit error pointing at the per-tool
// naming, instead of a silent fall-through to text-mode behaviour.
TEST_CASE("search_text rejects 'type' alias with per-tool-name suggestion",
          "[s6.17][tool_arg_validation]")
{
    auto tmp = make_tmp();
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::SearchTextTool tool;
    locus::ToolCall call{"c1", "search_text",
        {{"query", "x"}, {"type", "regex"}}};
    auto r = tool.execute(call, ws);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("unrecognized argument 'type'"));
    REQUIRE_THAT(r.content, ContainsSubstring("search_regex"));

    fs::remove_all(tmp);
}

TEST_CASE("search_text rejects 'mode' arg (Task G discriminator retired)",
          "[s6.17][tool_arg_validation]")
{
    auto tmp = make_tmp();
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::SearchTextTool tool;
    locus::ToolCall call{"c1", "search_text",
        {{"query", "x"}, {"mode", "text"}}};
    auto r = tool.execute(call, ws);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("unrecognized argument 'mode'"));

    fs::remove_all(tmp);
}

TEST_CASE("search_symbols keeps 'kind' as a real arg (not aliased)",
          "[s6.17][tool_arg_validation]")
{
    auto tmp = make_tmp();
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::SearchSymbolsTool tool;
    locus::ToolCall call{"c1", "search_symbols",
        {{"name", "Foo"}, {"kind", "class"}}};
    auto r = tool.execute(call, ws);
    // FakeWorkspaceServices has no real index -> tool returns its own error.
    // What matters: the result is NOT "unrecognized argument 'kind'".
    REQUIRE_THAT(r.content, !ContainsSubstring("unrecognized argument 'kind'"));

    fs::remove_all(tmp);
}

#ifdef _WIN32

TEST_CASE("run_command rejects 'cmd' alias with command suggestion",
          "[s6.17][tool_arg_validation]")
{
    auto tmp = make_tmp();
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::RunCommandTool tool;
    locus::ToolCall call{"c1", "run_command", {{"cmd", "echo hi"}}};
    auto r = tool.execute(call, ws);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("unrecognized argument 'cmd'"));
    REQUIRE_THAT(r.content, ContainsSubstring("'command'"));

    fs::remove_all(tmp);
}

#endif

TEST_CASE("write_file rejects 'body' alias with content suggestion",
          "[s6.17][tool_arg_validation]")
{
    auto tmp = make_tmp();
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::WriteFileTool tool;
    locus::ToolCall call{"c1", "write_file",
        {{"path", "x.txt"}, {"body", "hi"}}};
    auto r = tool.execute(call, ws);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("unrecognized argument 'body'"));
    REQUIRE_THAT(r.content, ContainsSubstring("'content'"));

    fs::remove_all(tmp);
}

TEST_CASE("reject_unknown_keys: unknown key without alias yields generic hint",
          "[s6.17][tool_arg_validation]")
{
    locus::ToolCall call{"c1", "read_file",
        {{"path", "x"}, {"totally_made_up", 1}}};
    auto err = locus::tools::reject_unknown_keys(call,
        {"path", "offset", "length"});
    REQUIRE(err.has_value());
    REQUIRE_THAT(err->content,
        ContainsSubstring("unrecognized argument 'totally_made_up'"));
    REQUIRE_THAT(err->content, ContainsSubstring("describe_tool"));
}

TEST_CASE("reject_unknown_keys: all-allowed call returns nullopt",
          "[s6.17][tool_arg_validation]")
{
    locus::ToolCall call{"c1", "read_file",
        {{"path", "x"}, {"offset", 1}, {"length", 100}}};
    auto err = locus::tools::reject_unknown_keys(call,
        {"path", "offset", "length"});
    REQUIRE_FALSE(err.has_value());
}
