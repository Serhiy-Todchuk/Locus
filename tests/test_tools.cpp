#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "tools/tool.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "core/workspace.h"
#include "index/index_query.h"
#include "support/fake_workspace_services.h"

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
    // S3.L: 4 search tools collapsed into a single unified `search` face (9 total).
    // S4.A: +1 `edit_file` (exact-string edits, single or atomic batch),
    //       -1 `create_file` (merged into `write_file` with optional `overwrite`).
    // S4.D: +2 `propose_plan`, `mark_step_done` (visible only in plan/execute modes).
    // S4.R: +2 `add_memory`, `search_memory` (memory bank).
    // S5.Z task 8: +1 `filter_output` (regex/substring over arbitrary text).
    // S6.11: +1 `describe_tool` (lazy-manifest meta-tool; always registered).
    REQUIRE(schema.size() == 19);

    // One `search` entry, no split search_* tools.
    bool has_search = false;
    for (auto& entry : schema) {
        if (entry["function"]["name"] == "search") has_search = true;
        REQUIRE(entry["function"]["name"] != "search_text");
        REQUIRE(entry["function"]["name"] != "search_symbols");
        REQUIRE(entry["function"]["name"] != "search_semantic");
        REQUIRE(entry["function"]["name"] != "search_hybrid");
    }
    REQUIRE(has_search);

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
    REQUIRE(all.size() == 19);  // S4.D adds propose_plan + mark_step_done;
                                // S4.R adds add_memory + search_memory;
                                // S5.Z task 8 adds filter_output;
                                // S6.11 adds describe_tool.
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

TEST_CASE("ToolRegistry: parse_tool_call repairs malformed JSON args",
          "[s6.10][json_repair][tool_registry]")
{
    // Trailing comma -- common Gemma / Qwen-Coder failure mode.
    auto a = locus::ToolRegistry::parse_tool_call(
        "call_1", "read_file", R"({"path":"src/main.cpp","offset":10,})");
    REQUIRE(a.args["path"] == "src/main.cpp");
    REQUIRE(a.args["offset"] == 10);

    // Unquoted keys + trailing comma combined.
    auto b = locus::ToolRegistry::parse_tool_call(
        "call_2", "read_file", R"({path: "src/x.cpp", offset: 5,})");
    REQUIRE(b.args["path"] == "src/x.cpp");
    REQUIRE(b.args["offset"] == 5);

    // Surrounding prose + missing closing brace.
    auto c = locus::ToolRegistry::parse_tool_call(
        "call_3", "read_file",
        R"(Sure! Here's the call: {"path":"src/y.cpp","offset":1)");
    REQUIRE(c.args["path"] == "src/y.cpp");
    REQUIRE(c.args["offset"] == 1);
}

TEST_CASE("ToolRegistry: parse_tool_call lifts field-name aliases",
          "[s6.10][tool_registry]")
{
    // Some models wrap args under `parameters` / `args` / `arguments` / `input`
    // instead of inlining them. parse_tool_call lifts the wrapper so downstream
    // tools see the canonical flat shape.
    auto a = locus::ToolRegistry::parse_tool_call(
        "call_1", "read_file",
        R"({"parameters":{"path":"src/main.cpp","offset":10}})");
    REQUIRE(a.args["path"] == "src/main.cpp");
    REQUIRE(a.args["offset"] == 10);
    REQUIRE_FALSE(a.args.contains("parameters"));

    for (const char* alias : {"args", "arguments", "input"}) {
        std::string body = std::string("{\"") + alias + "\":{\"path\":\"x\"}}";
        auto call = locus::ToolRegistry::parse_tool_call("c", "read_file", body);
        REQUIRE(call.args.contains("path"));
        REQUIRE(call.args["path"] == "x");
    }

    // Negative: don't hijack a tool that legitimately takes one of those
    // names as a real flat parameter. An alias is only lifted when the outer
    // object has EXACTLY one key AND its value is itself an object.
    auto literal = locus::ToolRegistry::parse_tool_call(
        "call_2", "fake", R"({"args":"some string value"})");
    REQUIRE(literal.args.contains("args"));
    REQUIRE(literal.args["args"] == "some string value");

    auto multi = locus::ToolRegistry::parse_tool_call(
        "call_3", "fake", R"({"parameters":{"path":"x"},"extra":1})");
    // outer has 2 keys -- alias NOT lifted; both keys still present.
    REQUIRE(multi.args.contains("parameters"));
    REQUIRE(multi.args.contains("extra"));
}

// -- ReadFileTool tests -----------------------------------------------------

TEST_CASE("ReadFileTool: basic read returns line-numbered content", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "hello.txt", "line1\nline2\nline3\nline4\nline5\n");

    locus::test::FakeWorkspaceServices ws{tmp};
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

    locus::test::FakeWorkspaceServices ws{tmp};
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

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::ReadFileTool tool;
    locus::ToolCall call{"c1", "read_file",
        {{"path", "../../etc/passwd"}}};

    auto result = tool.execute(call, ws);
    REQUIRE_FALSE(result.success);

    fs::remove_all(tmp);
}

// -- WriteFileTool tests ----------------------------------------------------

TEST_CASE("WriteFileTool: creates a new file with parent directories", "[s0.6][s4.a]")
{
    auto tmp = make_temp_workspace();

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::WriteFileTool tool;
    locus::ToolCall call{"c1", "write_file",
        {{"path", "subdir/new.txt"}, {"content", "hello"}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE(fs::exists(tmp / "subdir" / "new.txt"));
    REQUIRE(read_entire_file(tmp / "subdir" / "new.txt") == "hello");

    fs::remove_all(tmp);
}

TEST_CASE("WriteFileTool: refuses to overwrite when overwrite flag absent", "[s0.6][s4.a]")
{
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "target.txt", "original");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::WriteFileTool tool;
    locus::ToolCall call{"c1", "write_file",
        {{"path", "target.txt"}, {"content", "new"}}};  // overwrite defaults to false

    auto result = tool.execute(call, ws);
    REQUIRE_FALSE(result.success);
    REQUIRE(read_entire_file(tmp / "target.txt") == "original");

    fs::remove_all(tmp);
}

TEST_CASE("WriteFileTool: overwrites when overwrite=true", "[s0.6][s4.a]")
{
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "target.txt", "original");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::WriteFileTool tool;
    locus::ToolCall call{"c1", "write_file",
        {{"path", "target.txt"}, {"content", "new"}, {"overwrite", true}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE(read_entire_file(tmp / "target.txt") == "new");

    fs::remove_all(tmp);
}

TEST_CASE("WriteFileTool: overwrite=false on a fresh path still creates", "[s4.a]")
{
    // overwrite=false means "do not replace an existing file" -- it must not
    // block creation of a brand-new path.
    auto tmp = make_temp_workspace();

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::WriteFileTool tool;
    locus::ToolCall call{"c1", "write_file",
        {{"path", "fresh.txt"}, {"content", "hi"}, {"overwrite", false}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE(read_entire_file(tmp / "fresh.txt") == "hi");

    fs::remove_all(tmp);
}

// -- DeleteFileTool tests ---------------------------------------------------

TEST_CASE("DeleteFileTool: deletes existing file", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "doomed.txt", "goodbye");

    locus::test::FakeWorkspaceServices ws{tmp};
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

// -- Preview tests for tools whose args carry the most signal ---------------
// The chat panel renders preview() as a one-liner under the tool name; these
// guard against accidental regression to "tool name only".

TEST_CASE("SearchTool: preview surfaces mode and query", "[preview]")
{
    locus::SearchTool tool;
    locus::ToolCall call{"c1", "search",
        {{"query", "fts5 sanitise"}, {"mode", "hybrid"}}};

    auto preview = tool.preview(call);
    REQUIRE_THAT(preview, ContainsSubstring("hybrid"));
    REQUIRE_THAT(preview, ContainsSubstring("fts5 sanitise"));
}

TEST_CASE("SearchTool: preview defaults mode to text", "[preview]")
{
    locus::SearchTool tool;
    locus::ToolCall call{"c1", "search", {{"query", "hello"}}};

    auto preview = tool.preview(call);
    REQUIRE_THAT(preview, ContainsSubstring("text"));
    REQUIRE_THAT(preview, ContainsSubstring("hello"));
}

TEST_CASE("SearchTool: preview reads `name` for symbols mode", "[preview]")
{
    locus::SearchTool tool;
    locus::ToolCall call{"c1", "search",
        {{"mode", "symbols"}, {"name", "AgentCore"}}};

    auto preview = tool.preview(call);
    REQUIRE_THAT(preview, ContainsSubstring("symbols"));
    REQUIRE_THAT(preview, ContainsSubstring("AgentCore"));
}

TEST_CASE("SearchTool: preview truncates long queries", "[preview]")
{
    locus::SearchTool tool;
    std::string long_q(200, 'x');
    locus::ToolCall call{"c1", "search", {{"query", long_q}}};

    auto preview = tool.preview(call);
    REQUIRE(preview.size() < 200);
    REQUIRE_THAT(preview, ContainsSubstring("..."));
}

TEST_CASE("ListDirectoryTool: preview names the path", "[preview]")
{
    locus::ListDirectoryTool tool;
    locus::ToolCall call{"c1", "list_directory",
        {{"path", "src/agent"}, {"depth", 2}}};

    auto preview = tool.preview(call);
    REQUIRE_THAT(preview, ContainsSubstring("src/agent"));
    REQUIRE_THAT(preview, ContainsSubstring("depth"));
}

TEST_CASE("ListDirectoryTool: preview handles empty path", "[preview]")
{
    locus::ListDirectoryTool tool;
    locus::ToolCall call{"c1", "list_directory", {{"path", ""}}};

    auto preview = tool.preview(call);
    REQUIRE_THAT(preview, ContainsSubstring("workspace root"));
}

TEST_CASE("FilterOutputTool: preview shows mode and pattern", "[preview]")
{
    locus::FilterOutputTool tool;
    locus::ToolCall call{"c1", "filter_output",
        {{"pattern", "error:"}, {"mode", "substring"}, {"invert", true}}};

    auto preview = tool.preview(call);
    REQUIRE_THAT(preview, ContainsSubstring("substring"));
    REQUIRE_THAT(preview, ContainsSubstring("error:"));
    REQUIRE_THAT(preview, ContainsSubstring("invert"));
}

TEST_CASE("AddMemoryTool: preview truncates and flattens content", "[preview]")
{
    locus::AddMemoryTool tool;
    nlohmann::json args = {
        {"content", "build command is\ncmake --build build/release"},
        {"tags",    nlohmann::json::array({"build", "windows"})},
        {"pinned",  true},
    };
    locus::ToolCall call{"c1", "add_memory", args};

    auto preview = tool.preview(call);
    // Newline collapsed -> single space.
    REQUIRE(preview.find('\n') == std::string::npos);
    REQUIRE_THAT(preview, ContainsSubstring("build command"));
    REQUIRE_THAT(preview, ContainsSubstring("tags=2"));
    REQUIRE_THAT(preview, ContainsSubstring("pinned"));
}

TEST_CASE("SearchMemoryTool: preview shows query", "[preview]")
{
    locus::SearchMemoryTool tool;
    locus::ToolCall call{"c1", "search_memory",
        {{"query", "compaction strategy"}}};

    auto preview = tool.preview(call);
    REQUIRE_THAT(preview, ContainsSubstring("search_memory"));
    REQUIRE_THAT(preview, ContainsSubstring("compaction strategy"));
}

// -- RunCommandTool tests ---------------------------------------------------

#ifdef _WIN32

TEST_CASE("RunCommandTool: captures exit code 0", "[s0.6]")
{
    auto tmp = make_temp_workspace();
    locus::test::FakeWorkspaceServices ws{tmp};
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
    locus::test::FakeWorkspaceServices ws{tmp};
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
    locus::test::FakeWorkspaceServices ws{tmp};
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

// -- ListDirectoryTool tests ------------------------------------------------

TEST_CASE("ListDirectoryTool: returns indexed files at root with '.'", "[s0.6][s0.8]")
{
    auto tmp = fs::temp_directory_path() / "locus_test_listdir_tool";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    write_test_file(tmp, "readme.md", "# Hello");
    write_test_file(tmp, "main.cpp", "int main() {}");
    fs::create_directories(tmp / "src");
    write_test_file(tmp / "src", "util.cpp", "void util() {}");

    {
        // Build a real index so ListDirectoryTool has data to query.
        locus::Workspace ws(tmp);
        locus::IWorkspaceServices& ws_ctx = ws;

        locus::ListDirectoryTool tool;

        // With "." (what LLMs typically send)
        locus::ToolCall call_dot{"c1", "list_directory", {{"path", "."}}};
        auto result_dot = tool.execute(call_dot, ws_ctx);
        REQUIRE(result_dot.success);
        REQUIRE_THAT(result_dot.content, ContainsSubstring("readme.md"));
        REQUIRE_THAT(result_dot.content, ContainsSubstring("main.cpp"));
        REQUIRE_THAT(result_dot.content, ContainsSubstring("src"));

        // With empty path
        locus::ToolCall call_empty{"c2", "list_directory", {{"path", ""}}};
        auto result_empty = tool.execute(call_empty, ws_ctx);
        REQUIRE(result_empty.success);
        REQUIRE_THAT(result_empty.content, ContainsSubstring("readme.md"));

        // With no path arg at all (default)
        locus::ToolCall call_default{"c3", "list_directory", nlohmann::json::object()};
        auto result_default = tool.execute(call_default, ws_ctx);
        REQUIRE(result_default.success);
        REQUIRE_THAT(result_default.content, ContainsSubstring("readme.md"));

        // Subdirectory listing
        locus::ToolCall call_src{"c4", "list_directory", {{"path", "src"}}};
        auto result_src = tool.execute(call_src, ws_ctx);
        REQUIRE(result_src.success);
        REQUIRE_THAT(result_src.content, ContainsSubstring("util.cpp"));
    }
    // Workspace (and file watcher) destroyed before cleanup.
    fs::remove_all(tmp, ec);
}

// -- Approval policy checks -------------------------------------------------

// -- S3.L: context-gated tool exposure --------------------------------------

namespace {

// Stub tool for gating tests. Configurable predicates; no real execute path.
class StubGatedTool : public locus::ITool {
public:
    StubGatedTool(std::string name, bool available, locus::ToolMode visible_mode)
        : name_(std::move(name)), available_(available), visible_mode_(visible_mode) {}

    std::string name()        const override { return name_; }
    std::string description() const override { return "stub"; }
    std::vector<locus::ToolParam> params() const override { return {}; }
    locus::ToolResult execute(const locus::ToolCall&, locus::IWorkspaceServices&,
                               const std::atomic<bool>* = nullptr) override {
        return {true, "", ""};
    }

    bool available(locus::IWorkspaceServices&) const override { return available_; }
    bool visible_in_mode(locus::ToolMode mode) const override { return mode == visible_mode_; }

private:
    std::string     name_;
    bool            available_;
    locus::ToolMode visible_mode_;
};

} // namespace

TEST_CASE("ToolRegistry: build_schema_json filters by available() predicate", "[s3.l]")
{
    locus::ToolRegistry registry;
    registry.register_tool(std::make_unique<StubGatedTool>(
        "always_here", true, locus::ToolMode::agent));
    registry.register_tool(std::make_unique<StubGatedTool>(
        "never_here", false, locus::ToolMode::agent));

    locus::test::FakeWorkspaceServices ws{fs::temp_directory_path()};

    // Unfiltered overload still sees both -- backward compatibility for tests.
    REQUIRE(registry.build_schema_json().size() == 2);

    // Filtered overload drops the unavailable one.
    auto filtered = registry.build_schema_json(ws, locus::ToolMode::agent);
    REQUIRE(filtered.size() == 1);
    REQUIRE(filtered[0]["function"]["name"] == "always_here");
}

TEST_CASE("ToolRegistry: build_schema_json filters by visible_in_mode()", "[s3.l]")
{
    locus::ToolRegistry registry;
    registry.register_tool(std::make_unique<StubGatedTool>(
        "agent_only",    true, locus::ToolMode::agent));
    registry.register_tool(std::make_unique<StubGatedTool>(
        "plan_only",     true, locus::ToolMode::plan));
    registry.register_tool(std::make_unique<StubGatedTool>(
        "subagent_only", true, locus::ToolMode::subagent));

    locus::test::FakeWorkspaceServices ws{fs::temp_directory_path()};

    auto agent    = registry.build_schema_json(ws, locus::ToolMode::agent);
    auto plan     = registry.build_schema_json(ws, locus::ToolMode::plan);
    auto subagent = registry.build_schema_json(ws, locus::ToolMode::subagent);

    REQUIRE(agent.size()    == 1);
    REQUIRE(plan.size()     == 1);
    REQUIRE(subagent.size() == 1);
    REQUIRE(agent[0]["function"]["name"]    == "agent_only");
    REQUIRE(plan[0]["function"]["name"]     == "plan_only");
    REQUIRE(subagent[0]["function"]["name"] == "subagent_only");
}

TEST_CASE("ITool defaults: available()=true, visible_in_mode only in agent", "[s3.l]")
{
    // Built-in tools use the default predicates -- verify the defaults match
    // today's behavior so existing callers don't silently change.
    locus::ToolRegistry registry;
    locus::register_builtin_tools(registry);

    locus::test::FakeWorkspaceServices ws{fs::temp_directory_path()};

    auto agent = registry.build_schema_json(ws, locus::ToolMode::agent);
    auto plan  = registry.build_schema_json(ws, locus::ToolMode::plan);

    // 9 baseline + 4 background-process tools (S4.I). The bg tools mark
    // themselves unavailable when the workspace has no ProcessRegistry,
    // which the FakeWorkspaceServices used here does not. plan-mode and
    // execute-mode-only tools (S4.D propose_plan, mark_step_done) are also
    // hidden from agent mode. S5.Z task 8 adds filter_output.
    REQUIRE(agent.size() == 10);
    REQUIRE(plan.size()  == 1);  // S4.D: propose_plan is the one plan-mode tool
}

// -- Approval policies ------------------------------------------------------

TEST_CASE("Tool approval policies are correct", "[s0.6]")
{
    locus::ToolRegistry registry;
    locus::register_builtin_tools(registry);

    // auto tools (read-only)
    REQUIRE(registry.find("read_file")->approval_policy() == locus::ToolApprovalPolicy::auto_approve);
    REQUIRE(registry.find("list_directory")->approval_policy() == locus::ToolApprovalPolicy::auto_approve);
    REQUIRE(registry.find("search")->approval_policy() == locus::ToolApprovalPolicy::auto_approve);
    REQUIRE(registry.find("get_file_outline")->approval_policy() == locus::ToolApprovalPolicy::auto_approve);

    // ask tools (mutating)
    REQUIRE(registry.find("write_file")->approval_policy() == locus::ToolApprovalPolicy::ask);
    REQUIRE(registry.find("edit_file")->approval_policy() == locus::ToolApprovalPolicy::ask);
    REQUIRE(registry.find("delete_file")->approval_policy() == locus::ToolApprovalPolicy::ask);
    REQUIRE(registry.find("run_command")->approval_policy() == locus::ToolApprovalPolicy::ask);

    // S4.I -- background-process tools.
    REQUIRE(registry.find("run_command_bg")->approval_policy() == locus::ToolApprovalPolicy::ask);
    REQUIRE(registry.find("read_process_output")->approval_policy() == locus::ToolApprovalPolicy::auto_approve);
    REQUIRE(registry.find("stop_process")->approval_policy() == locus::ToolApprovalPolicy::ask);
    REQUIRE(registry.find("list_processes")->approval_policy() == locus::ToolApprovalPolicy::auto_approve);
}

// -- EditFileTool tests (S4.A) ---------------------------------------------

#include "tools/shared.h"

namespace {

// Helper: run read_file so the ReadTracker accepts subsequent edits.
void prime_read(const fs::path& tmp, const std::string& rel)
{
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::ReadFileTool reader;
    locus::ToolCall call{"r", "read_file", {{"path", rel}}};
    auto r = reader.execute(call, ws);
    REQUIRE(r.success);
}

// Build an edit_file call with a single (old, new, replace_all?) edit.
locus::ToolCall one_edit(const std::string& rel, const std::string& old_s,
                         const std::string& new_s, bool replace_all = false)
{
    nlohmann::json edit = {{"old_string", old_s}, {"new_string", new_s}};
    if (replace_all) edit["replace_all"] = true;
    return {"c1", "edit_file",
            {{"path", rel}, {"edits", nlohmann::json::array({edit})}}};
}

} // namespace

TEST_CASE("EditFileTool: exact single-occurrence replacement", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "f.txt", "alpha beta gamma");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    auto result = tool.execute(one_edit("f.txt", "beta", "BETA"), ws);
    REQUIRE(result.success);
    REQUIRE(read_entire_file(tmp / "f.txt") == "alpha BETA gamma");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: ambiguous match fails uniqueness check", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "f.txt", "foo foo foo");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    auto result = tool.execute(one_edit("f.txt", "foo", "bar"), ws);
    REQUIRE_FALSE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("matches 3 locations"));
    // File must be untouched on failure.
    REQUIRE(read_entire_file(tmp / "f.txt") == "foo foo foo");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: replace_all rewrites every occurrence", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "f.txt", "foo foo foo");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    auto result = tool.execute(one_edit("f.txt", "foo", "bar", true), ws);
    REQUIRE(result.success);
    REQUIRE(read_entire_file(tmp / "f.txt") == "bar bar bar");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: no match fails cleanly", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "f.txt", "abc def");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    auto result = tool.execute(one_edit("f.txt", "xyz", "zzz"), ws);
    REQUIRE_FALSE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("not found"));
    REQUIRE(read_entire_file(tmp / "f.txt") == "abc def");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: preserves indentation exactly", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    // Tabs, spaces, mixed -- the exact-match rule must keep them intact.
    std::string body = "fn main() {\n\treturn 0;\n}\n";
    write_test_file(tmp, "m.rs", body);
    prime_read(tmp, "m.rs");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    auto result = tool.execute(
        one_edit("m.rs", "\treturn 0;", "\treturn 42;"), ws);
    REQUIRE(result.success);
    REQUIRE(read_entire_file(tmp / "m.rs") == "fn main() {\n\treturn 42;\n}\n");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: requires prior read_file in session", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "unseen.txt", "hello");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    auto result = tool.execute(one_edit("unseen.txt", "hello", "world"), ws);
    REQUIRE_FALSE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("read the file first"));
    REQUIRE(read_entire_file(tmp / "unseen.txt") == "hello");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: a prior write_file counts as having seen the file",
          "[s4.a]")
{
    // The agent authored every byte via write_file, so the read-before-edit
    // guard must let a follow-up edit_file through without an intervening
    // read_file. Forcing a read_file here would burn a wasted round trip.
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::WriteFileTool writer;
    locus::ToolCall write_call{"w", "write_file",
        {{"path", "fresh.txt"}, {"content", "hello"}}};
    auto w = writer.execute(write_call, ws);
    REQUIRE(w.success);

    locus::EditFileTool editor;
    auto e = editor.execute(one_edit("fresh.txt", "hello", "world"), ws);
    REQUIRE(e.success);
    REQUIRE(read_entire_file(tmp / "fresh.txt") == "world");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: rejects old_string == new_string", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "f.txt", "hello");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    auto result = tool.execute(one_edit("f.txt", "hello", "hello"), ws);
    REQUIRE_FALSE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("no-op"));

    fs::remove_all(tmp);
}

// -- Batch (edits[] with multiple entries) -----------------------------------

TEST_CASE("EditFileTool: applies every batched edit in order", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "f.txt", "red green blue");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    locus::ToolCall call{"c1", "edit_file",
        {{"path", "f.txt"},
         {"edits", nlohmann::json::array({
             {{"old_string", "red"},   {"new_string", "R"}},
             {{"old_string", "green"}, {"new_string", "G"}},
             {{"old_string", "blue"},  {"new_string", "B"}},
         })}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE(read_entire_file(tmp / "f.txt") == "R G B");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: batch is atomic - one bad edit aborts all", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "f.txt", "red green blue");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    locus::ToolCall call{"c1", "edit_file",
        {{"path", "f.txt"},
         {"edits", nlohmann::json::array({
             {{"old_string", "red"},     {"new_string", "R"}},
             {{"old_string", "MISSING"}, {"new_string", "X"}},  // will fail
             {{"old_string", "blue"},    {"new_string", "B"}},
         })}}};

    auto result = tool.execute(call, ws);
    REQUIRE_FALSE(result.success);
    REQUIRE_THAT(result.content, ContainsSubstring("edit 2/3"));
    // File untouched -- atomicity.
    REQUIRE(read_entire_file(tmp / "f.txt") == "red green blue");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: later batched edits see earlier results", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "f.txt", "one");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    locus::ToolCall call{"c1", "edit_file",
        {{"path", "f.txt"},
         {"edits", nlohmann::json::array({
             {{"old_string", "one"}, {"new_string", "two"}},
             {{"old_string", "two"}, {"new_string", "three"}},
         })}}};

    auto result = tool.execute(call, ws);
    REQUIRE(result.success);
    REQUIRE(read_entire_file(tmp / "f.txt") == "three");

    fs::remove_all(tmp);
}

TEST_CASE("EditFileTool: empty edits array rejected", "[s4.a]")
{
    locus::tools::ReadTracker::instance().clear();
    auto tmp = make_temp_workspace();
    write_test_file(tmp, "f.txt", "hello");
    prime_read(tmp, "f.txt");

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    locus::ToolCall call{"c1", "edit_file",
        {{"path", "f.txt"}, {"edits", nlohmann::json::array()}}};

    auto result = tool.execute(call, ws);
    REQUIRE_FALSE(result.success);

    fs::remove_all(tmp);
}
