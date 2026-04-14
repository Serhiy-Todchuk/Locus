#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "tool.h"
#include "tool_registry.h"
#include "tools.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

// -- Helpers ----------------------------------------------------------------

// Create a temp directory for tool tests. Cleaned up by caller.
static fs::path make_temp_workspace()
{
    auto tmp = fs::temp_directory_path() / "locus_test_tools";
    fs::create_directories(tmp);
    return tmp;
}

static void write_test_file(const fs::path& dir, const std::string& name,
                            const std::string& content)
{
    std::ofstream out(dir / name, std::ios::trunc);
    out << content;
}

static std::string read_entire_file(const fs::path& p)
{
    std::ifstream in(p);
    return {std::istreambuf_iterator<char>(in), {}};
}

// -- Schema tests -----------------------------------------------------------

TEST_CASE("ToolRegistry: build_schema_json produces valid OpenAI schema", "[s0.6]")
{
    locus::ToolRegistry registry;
    locus::register_builtin_tools(registry);

    auto schema = registry.build_schema_json();

    REQUIRE(schema.is_array());
    REQUIRE(schema.size() == 9);  // 9 built-in tools

    for (auto& entry : schema) {
        REQUIRE(entry.contains("type"));
        REQUIRE(entry["type"] == "function");
        REQUIRE(entry.contains("function"));

        auto& func = entry["function"];
        REQUIRE(func.contains("name"));
        REQUIRE(func.contains("description"));
        REQUIRE(func.contains("parameters"));
        REQUIRE(func["parameters"]["type"] == "object");
        REQUIRE(func["parameters"].contains("properties"));
    }
}

TEST_CASE("ToolRegistry: find returns correct tool", "[s0.6]")
{
    locus::ToolRegistry registry;
    locus::register_builtin_tools(registry);

    REQUIRE(registry.find("read_file") != nullptr);
    REQUIRE(registry.find("read_file")->name() == "read_file");
    REQUIRE(registry.find("nonexistent") == nullptr);
}

TEST_CASE("ToolRegistry: all returns all tools", "[s0.6]")
{
    locus::ToolRegistry registry;
    locus::register_builtin_tools(registry);

    auto all = registry.all();
    REQUIRE(all.size() == 9);
}

TEST_CASE("ToolRegistry: parse_tool_call handles valid and empty JSON", "[s0.6]")
{
    auto call = locus::ToolRegistry::parse_tool_call(
        "call_1", "read_file", R"({"path":"src/main.cpp","offset":10})");
    REQUIRE(call.id == "call_1");
    REQUIRE(call.tool_name == "read_file");
    REQUIRE(call.args["path"] == "src/main.cpp");
    REQUIRE(call.args["offset"] == 10);

    auto empty = locus::ToolRegistry::parse_tool_call("call_2", "foo", "");
    REQUIRE(empty.args.is_object());
    REQUIRE(empty.args.empty());

    auto bad = locus::ToolRegistry::parse_tool_call("call_3", "foo", "not json");
    REQUIRE(bad.args.is_object());
}

// -- ReadFileTool tests -----------------------------------------------------

TEST_CASE("ReadFileTool: basic read returns line-numbered content", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "hello.txt", "line1\nline2\nline3\nline4\nline5\n");

    locus::WorkspaceContext ws{tmp, nullptr};
    locus::ReadFileTool tool;
    locus::ToolCall call{"c1", "read_file", {{"path", "hello.txt"}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("1\tline1"));
    REQUIRE_THAT(result.content, ContainsSubstring("5\tline5"));
    REQUIRE_THAT(result.content, ContainsSubstring("of 5"));

    fs::remove_all(tmp);
}

TEST_CASE("ReadFileTool: pagination with offset and length", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    std::string content;
    for (int i = 1; i <= 50; ++i)
        content += "line " + std::to_string(i) + "\n";
    write_test_file(tmp, "big.txt", content);

    locus::WorkspaceContext ws{tmp, nullptr};
    locus::ReadFileTool tool;

    // Read lines 10-14
    locus::ToolCall call{"c1", "read_file",
        {{"path", "big.txt"}, {"offset", 10}, {"length", 5}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("10\tline 10"));
    REQUIRE_THAT(result.content, ContainsSubstring("14\tline 14"));
    REQUIRE_THAT(result.content, ContainsSubstring("lines 10-14 of 50"));

    // Offset past end
    locus::ToolCall call2{"c2", "read_file",
        {{"path", "big.txt"}, {"offset", 999}}};
    auto result2 = tool.execute(call2, ws);
    REQUIRE_FALSE(result2.success);

    fs::remove_all(tmp);
}

TEST_CASE("ReadFileTool: rejects path traversal", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "ok.txt", "ok");

    locus::WorkspaceContext ws{tmp, nullptr};
    locus::ReadFileTool tool;
    locus::ToolCall call{"c1", "read_file",
        {{"path", "../../etc/passwd"}}};

    auto result = tool.execute(call, ws);
    REQUIRE_FALSE(result.success);

    fs::remove_all(tmp);
}

// -- WriteFileTool tests ----------------------------------------------------

TEST_CASE("WriteFileTool: overwrites existing file", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "target.txt", "original content");

    locus::WorkspaceContext ws{tmp, nullptr};
    locus::WriteFileTool tool;
    locus::ToolCall call{"c1", "write_file",
        {{"path", "target.txt"}, {"content", "new content"}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE(read_entire_file(tmp / "target.txt") == "new content");

    fs::remove_all(tmp);
}

TEST_CASE("WriteFileTool: fails for nonexistent file", "[s0.6]")
{
    auto tmp = make_temp_workspace();

    locus::WorkspaceContext ws{tmp, nullptr};
    locus::WriteFileTool tool;
    locus::ToolCall call{"c1", "write_file",
        {{"path", "nope.txt"}, {"content", "data"}}};

    auto result = tool.execute(call, ws);
    REQUIRE_FALSE(result.success);

    fs::remove_all(tmp);
}

// -- CreateFileTool tests ---------------------------------------------------

TEST_CASE("CreateFileTool: creates new file with directories", "[s0.6]")
{
    auto tmp = make_temp_workspace();

    locus::WorkspaceContext ws{tmp, nullptr};
    locus::CreateFileTool tool;
    locus::ToolCall call{"c1", "create_file",
        {{"path", "subdir/new.txt"}, {"content", "hello"}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE(fs::exists(tmp / "subdir" / "new.txt"));
    REQUIRE(read_entire_file(tmp / "subdir" / "new.txt") == "hello");

    fs::remove_all(tmp);
}

TEST_CASE("CreateFileTool: fails if file exists", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "exists.txt", "old");

    locus::WorkspaceContext ws{tmp, nullptr};
    locus::CreateFileTool tool;
    locus::ToolCall call{"c1", "create_file",
        {{"path", "exists.txt"}, {"content", "new"}}};

    auto result = tool.execute(call, ws);
    REQUIRE_FALSE(result.success);

    fs::remove_all(tmp);
}

// -- DeleteFileTool tests ---------------------------------------------------

TEST_CASE("DeleteFileTool: deletes existing file", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "doomed.txt", "goodbye");

    locus::WorkspaceContext ws{tmp, nullptr};
    locus::DeleteFileTool tool;
    locus::ToolCall call{"c1", "delete_file", {{"path", "doomed.txt"}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE_FALSE(fs::exists(tmp / "doomed.txt"));

    fs::remove_all(tmp);
}

TEST_CASE("DeleteFileTool: preview contains warning", "[s0.6]")
{
    locus::DeleteFileTool tool;
    locus::ToolCall call{"c1", "delete_file", {{"path", "important.cpp"}}};

    auto preview = tool.preview(call);
    REQUIRE_THAT(preview, ContainsSubstring("WARNING"));
    REQUIRE_THAT(preview, ContainsSubstring("important.cpp"));
}

// -- RunCommandTool tests ---------------------------------------------------

#ifdef _WIN32

TEST_CASE("RunCommandTool: captures exit code 0", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    locus::WorkspaceContext ws{tmp, nullptr};
    locus::RunCommandTool tool;
    locus::ToolCall call{"c1", "run_command",
        {{"command", "echo hello"}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("exit code: 0"));
    REQUIRE_THAT(result.content, ContainsSubstring("hello"));

    fs::remove_all(tmp);
}

TEST_CASE("RunCommandTool: captures nonzero exit code", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    locus::WorkspaceContext ws{tmp, nullptr};
    locus::RunCommandTool tool;
    locus::ToolCall call{"c1", "run_command",
        {{"command", "exit /b 42"}}};

    auto result = tool.execute(call, ws);
    REQUIRE_FALSE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("exit code: 42"));

    fs::remove_all(tmp);
}

TEST_CASE("RunCommandTool: timeout kills process", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    locus::WorkspaceContext ws{tmp, nullptr};
    locus::RunCommandTool tool;
    // ping with -w 1000 -n 120 will take ~120 seconds; we kill it after 1.5s
    locus::ToolCall call{"c1", "run_command",
        {{"command", "ping -n 120 -w 1000 127.0.0.1"}, {"timeout_ms", 1500}}};

    auto result = tool.execute(call, ws);
    REQUIRE_FALSE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("TIMEOUT"));

    fs::remove_all(tmp);
}

#endif

// -- Approval policy checks -------------------------------------------------

TEST_CASE("Tool approval policies are correct", "[s0.6]")
{
    locus::ToolRegistry registry;
    locus::register_builtin_tools(registry);

    // auto tools (read-only)
    REQUIRE(registry.find("read_file")->approval_policy() == "auto");
    REQUIRE(registry.find("list_directory")->approval_policy() == "auto");
    REQUIRE(registry.find("search_text")->approval_policy() == "auto");
    REQUIRE(registry.find("search_symbols")->approval_policy() == "auto");
    REQUIRE(registry.find("get_file_outline")->approval_policy() == "auto");

    // always tools (mutating)
    REQUIRE(registry.find("write_file")->approval_policy() == "always");
    REQUIRE(registry.find("create_file")->approval_policy() == "always");
    REQUIRE(registry.find("delete_file")->approval_policy() == "always");
    REQUIRE(registry.find("run_command")->approval_policy() == "always");
}
