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

// search_symbols gained a max_results arg (ADR-0009 follow-up to S6.17 G).
// Verify the key passes reject_unknown_keys.
TEST_CASE("search_symbols accepts max_results (post-ADR-0009)",
          "[tool_arg_validation]")
{
    auto tmp = make_tmp();
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::SearchSymbolsTool tool;
    locus::ToolCall call{"c1", "search_symbols",
        {{"name", "Foo"}, {"max_results", 5}}};
    auto r = tool.execute(call, ws);
    REQUIRE_THAT(r.content, !ContainsSubstring("unrecognized argument 'max_results'"));

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

// -- Schema-on-error safety net --------------------------------------------
//
// When tools pass `this` as the third arg, the canonical schema is appended
// to the error so the model can self-correct on the next round without a
// separate `describe_tool` call. Critical for lazy_tool_manifest -- the
// model never had the full schema in its manifest.

TEST_CASE("render_tool_schema emits name + description + parameters",
          "[schema_on_error]")
{
    locus::EditFileTool tool;
    std::string s = locus::tools::render_tool_schema(tool);
    REQUIRE_THAT(s, ContainsSubstring("\"name\""));
    REQUIRE_THAT(s, ContainsSubstring("edit_file"));
    REQUIRE_THAT(s, ContainsSubstring("\"description\""));
    REQUIRE_THAT(s, ContainsSubstring("\"parameters\""));
    REQUIRE_THAT(s, ContainsSubstring("\"properties\""));
    REQUIRE_THAT(s, ContainsSubstring("\"required\""));
    REQUIRE_THAT(s, ContainsSubstring("file_path"));
    REQUIRE_THAT(s, ContainsSubstring("edits_array"));
}

TEST_CASE("missing_required_arg appends schema + names the arg",
          "[schema_on_error]")
{
    locus::EditFileTool tool;
    auto r = locus::tools::missing_required_arg(tool, "file_path",
        "the workspace-relative path to the file to edit");
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("missing required argument"));
    REQUIRE_THAT(r.content, ContainsSubstring("'file_path'"));
    REQUIRE_THAT(r.content,
        ContainsSubstring("the workspace-relative path to the file to edit"));
    REQUIRE_THAT(r.content, ContainsSubstring("Tool schema for retry"));
    REQUIRE_THAT(r.content, ContainsSubstring("edits_array"));
}

TEST_CASE("error_with_schema appends schema after the message",
          "[schema_on_error]")
{
    locus::EditFileTool tool;
    auto r = locus::tools::error_with_schema(
        "Error: 'edits_array' must be an array.", tool);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content,
        ContainsSubstring("'edits_array' must be an array"));
    REQUIRE_THAT(r.content, ContainsSubstring("Tool schema for retry"));
    REQUIRE_THAT(r.content, ContainsSubstring("file_path"));
}

TEST_CASE("reject_unknown_keys with tool pointer injects schema, not describe_tool hint",
          "[schema_on_error]")
{
    locus::EditFileTool tool;
    locus::ToolCall call{"c1", "edit_file",
        {{"file_path", "x"}, {"bogus_key", 1}}};
    auto err = locus::tools::reject_unknown_keys(call,
        {"file_path", "edits_array"}, &tool);
    REQUIRE(err.has_value());
    REQUIRE_THAT(err->content,
        ContainsSubstring("unrecognized argument 'bogus_key'"));
    REQUIRE_THAT(err->content, ContainsSubstring("Tool schema for retry"));
    REQUIRE_THAT(err->content, ContainsSubstring("file_path"));
    // describe_tool hint is NOT in the schema-injection path -- the schema
    // is the proactive equivalent of what describe_tool would have returned.
    REQUIRE_FALSE(err->content.find("describe_tool") != std::string::npos);
}

TEST_CASE("reject_unknown_keys without tool pointer keeps legacy describe_tool hint",
          "[schema_on_error]")
{
    locus::ToolCall call{"c1", "read_file",
        {{"path", "x"}, {"bogus", 1}}};
    auto err = locus::tools::reject_unknown_keys(call,
        {"path", "offset", "length"});
    REQUIRE(err.has_value());
    REQUIRE_THAT(err->content, ContainsSubstring("describe_tool"));
    REQUIRE_FALSE(err->content.find("Tool schema for retry") != std::string::npos);
}

TEST_CASE("edit_file end-to-end: missing file_path returns schema in error",
          "[schema_on_error]")
{
    auto tmp = make_tmp();
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::EditFileTool tool;
    // Canonical form -- edits_array present, file_path missing.
    locus::ToolCall call{"c1", "edit_file",
        {{"edits_array", nlohmann::json::array({
            nlohmann::json{{"old_string", "a"}, {"new_string", "b"}}
        })}}};
    auto r = tool.execute(call, ws);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("missing required argument"));
    REQUIRE_THAT(r.content, ContainsSubstring("'file_path'"));
    REQUIRE_THAT(r.content, ContainsSubstring("Tool schema for retry"));
    REQUIRE_THAT(r.content, ContainsSubstring("edits_array"));
    fs::remove_all(tmp);
}

// -- coerce_bool: type-tolerant boolean tool-arg read -----------------------
//
// Regression for the agentic DX12 session (2026-06-06): qwen3-coder-480b
// emitted `"overwrite":"True"` (Python-style string bool) on a write_file
// call. WriteFileTool::preview ran `call.args.value("overwrite", false)` on
// the UI thread during the tool-pending broadcast; nlohmann throws type_error
// on a string->bool conversion, the throw escaped the UI-thread call site,
// and locus_gui.exe fail-fasted (0xc0000409). coerce_bool never throws.

TEST_CASE("coerce_bool accepts real bool, string forms, ints; never throws",
          "[tool_arg_validation][coerce_bool]")
{
    using locus::tools::coerce_bool;

    // Real JSON bool passes through.
    REQUIRE(coerce_bool(nlohmann::json{{"k", true}},  "k", false) == true);
    REQUIRE(coerce_bool(nlohmann::json{{"k", false}}, "k", true)  == false);

    // The crash case: Python-style capitalized string booleans.
    REQUIRE(coerce_bool(nlohmann::json{{"k", "True"}},  "k", false) == true);
    REQUIRE(coerce_bool(nlohmann::json{{"k", "False"}}, "k", true)  == false);

    // Lowercase + numeric-string + yes/no/on/off.
    REQUIRE(coerce_bool(nlohmann::json{{"k", "true"}},  "k", false) == true);
    REQUIRE(coerce_bool(nlohmann::json{{"k", "1"}},     "k", false) == true);
    REQUIRE(coerce_bool(nlohmann::json{{"k", "yes"}},   "k", false) == true);
    REQUIRE(coerce_bool(nlohmann::json{{"k", "on"}},    "k", false) == true);
    REQUIRE(coerce_bool(nlohmann::json{{"k", "0"}},     "k", true)  == false);
    REQUIRE(coerce_bool(nlohmann::json{{"k", "off"}},   "k", true)  == false);

    // Integer / float forms.
    REQUIRE(coerce_bool(nlohmann::json{{"k", 1}},   "k", false) == true);
    REQUIRE(coerce_bool(nlohmann::json{{"k", 0}},   "k", true)  == false);
    REQUIRE(coerce_bool(nlohmann::json{{"k", 2.5}}, "k", false) == true);

    // Missing key / null / unparseable -> fallback (and no throw).
    REQUIRE(coerce_bool(nlohmann::json::object(),       "k", true)  == true);
    REQUIRE(coerce_bool(nlohmann::json{{"k", nullptr}}, "k", true)  == true);
    REQUIRE(coerce_bool(nlohmann::json{{"k", "maybe"}}, "k", false) == false);
    REQUIRE(coerce_bool(nlohmann::json{{"k",
        nlohmann::json::array({1, 2})}}, "k", true) == true);
}

TEST_CASE("write_file: string 'True' overwrite does not throw in preview or execute",
          "[tool_arg_validation][coerce_bool]")
{
    auto tmp = make_tmp();
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::WriteFileTool tool;

    // Pre-existing file so the overwrite flag is actually load-bearing.
    write_file(tmp, "main.cpp", "old\n");

    locus::ToolCall call{"c1", "write_file",
        {{"path", "main.cpp"}, {"content", "new content\n"},
         {"overwrite", "True"}}};

    // preview() is the UI-thread path that crashed -- must not throw.
    std::string prev;
    REQUIRE_NOTHROW(prev = tool.preview(call));
    REQUIRE_THAT(prev, ContainsSubstring("overwrite"));

    // execute() must honour the string-bool and overwrite, not error out.
    locus::ToolResult r;
    REQUIRE_NOTHROW(r = tool.execute(call, ws));
    REQUIRE(r.success);

    std::string got;
    {
        std::ifstream in(tmp / "main.cpp", std::ios::binary);
        got.assign((std::istreambuf_iterator<char>(in)), {});
    }
    REQUIRE_THAT(got, ContainsSubstring("new content"));

    fs::remove_all(tmp);
}

// -- coerce_json_array: tolerant array tool-arg read ------------------------
//
// Regression for the agentic DX12 session (2026-06-06): qwen3-coder-480b
// emitted `"edits":"[{...}]"` -- the edits array double-encoded as a JSON
// STRING. EditFileTool::preview ran `call.args.value("edits", json::array())`
// on the agent thread; nlohmann throws type_error (default array vs stored
// string) and the process fail-fasted (0xc0000409). coerce_json_array never
// throws and unwraps the stringified array.

TEST_CASE("coerce_json_array unwraps real array, JSON-string array; never throws",
          "[tool_arg_validation][coerce_json_array]")
{
    using locus::tools::coerce_json_array;

    // Real array passes through.
    auto real = nlohmann::json{{"edits", nlohmann::json::array({
        nlohmann::json{{"old_string", "a"}, {"new_string", "b"}}})}};
    auto r1 = coerce_json_array(real, "edits");
    REQUIRE(r1.is_array());
    REQUIRE(r1.size() == 1);
    REQUIRE(r1[0]["old_string"] == "a");

    // The crash case: array double-encoded as a JSON string.
    auto str = nlohmann::json{{"edits",
        "[{\"old_string\":\"x\",\"new_string\":\"y\"}]"}};
    auto r2 = coerce_json_array(str, "edits");
    REQUIRE(r2.is_array());
    REQUIRE(r2.size() == 1);
    REQUIRE(r2[0]["new_string"] == "y");

    // Missing / null / non-array-string -> empty array, no throw.
    REQUIRE(coerce_json_array(nlohmann::json::object(), "edits").empty());
    REQUIRE(coerce_json_array(nlohmann::json{{"edits", nullptr}}, "edits").empty());
    REQUIRE(coerce_json_array(nlohmann::json{{"edits", "not json"}}, "edits").empty());
    REQUIRE(coerce_json_array(nlohmann::json{{"edits", 42}}, "edits").empty());
    // A string that parses to a NON-array (object) -> empty array.
    REQUIRE(coerce_json_array(nlohmann::json{{"edits", "{\"a\":1}"}}, "edits").empty());
}

// Observed in the wild (plan_mode UIA run, gemma-4-12b-qat): the model
// double-encoded `steps` as a string AND used Python-dict single quotes, so
// strict JSON parse failed and propose_plan rejected the call. coerce_json_array
// now routes a failed strict parse through repair_for_parse, so the single-
// quoted body recovers instead of dropping.
TEST_CASE("coerce_json_array repairs Python-dict-style single-quoted string array",
          "[tool_arg_validation][coerce_json_array]")
{
    using locus::tools::coerce_json_array;

    auto pyish = nlohmann::json{{"steps",
        "[{'description': 'Create hello.py', 'tools_needed': ['write_file']}, "
        "{'description': 'Run it', 'tools_needed': ['run_command']}]"}};
    auto r = coerce_json_array(pyish, "steps");
    REQUIRE(r.is_array());
    REQUIRE(r.size() == 2);
    REQUIRE(r[0]["description"] == "Create hello.py");
    REQUIRE(r[1]["tools_needed"][0] == "run_command");
}

// -- coerce_int: tolerant integer tool-arg read -----------------------------
//
// Regression for the agentic DX12 session (2026-06-06): qwen3-coder-480b sent
// read_file with offset/length as STRINGS. `json::value<int>` threw type_error;
// execute()'s catch-all turned it into an "internal error" result, the model
// re-sent the same call, and the QualityMonitor repeated-tool-call cap aborted
// the turn after 2 nudges. coerce_int never throws and parses the string.

TEST_CASE("coerce_int accepts int, float, numeric string; never throws",
          "[tool_arg_validation][coerce_int]")
{
    using locus::tools::coerce_int;

    REQUIRE(coerce_int(nlohmann::json{{"n", 42}},     "n", 7) == 42);
    REQUIRE(coerce_int(nlohmann::json{{"n", 42.9}},   "n", 7) == 42);   // truncates
    REQUIRE(coerce_int(nlohmann::json{{"n", "100"}},  "n", 7) == 100);  // the crash case
    REQUIRE(coerce_int(nlohmann::json{{"n", " 100 "}},"n", 7) == 100);
    REQUIRE(coerce_int(nlohmann::json{{"n", "-5"}},   "n", 7) == -5);
    REQUIRE(coerce_int(nlohmann::json{{"n", "100px"}},"n", 7) == 100);  // trailing junk
    REQUIRE(coerce_int(nlohmann::json{{"n", true}},   "n", 7) == 1);

    // Missing / null / non-numeric -> fallback, no throw.
    REQUIRE(coerce_int(nlohmann::json::object(),        "n", 7) == 7);
    REQUIRE(coerce_int(nlohmann::json{{"n", nullptr}},  "n", 7) == 7);
    REQUIRE(coerce_int(nlohmann::json{{"n", "abc"}},    "n", 7) == 7);
    REQUIRE(coerce_int(nlohmann::json{{"n",
        nlohmann::json::array({1})}}, "n", 7) == 7);
}

TEST_CASE("read_file: string offset/length do not throw and apply",
          "[tool_arg_validation][coerce_int]")
{
    auto tmp = make_tmp();
    write_file(tmp, "f.txt", "l1\nl2\nl3\nl4\nl5\n");
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::ReadFileTool tool;

    // offset + length sent as STRINGS (the failure shape).
    locus::ToolCall call{"c1", "read_file",
        {{"path", "f.txt"}, {"offset", "2"}, {"length", "2"}}};

    REQUIRE_NOTHROW(tool.preview(call));
    locus::ToolResult r;
    REQUIRE_NOTHROW(r = tool.execute(call, ws));
    REQUIRE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("l2"));
    REQUIRE_THAT(r.content, ContainsSubstring("l3"));

    fs::remove_all(tmp);
}

TEST_CASE("coerce_string accepts string, coerces number/bool, null->fallback; never throws",
          "[tool_arg_validation][coerce_string]")
{
    using locus::tools::coerce_string;

    REQUIRE(coerce_string(nlohmann::json{{"s", "hi"}}, "s") == "hi");
    // The crash case: key PRESENT but null. json::value<string>("s","") THROWS
    // type_error.302 here; coerce_string returns the fallback instead.
    REQUIRE(coerce_string(nlohmann::json{{"s", nullptr}}, "s") == "");
    REQUIRE(coerce_string(nlohmann::json{{"s", nullptr}}, "s", "def") == "def");
    // Key absent -> fallback.
    REQUIRE(coerce_string(nlohmann::json::object(), "s", "def") == "def");
    // Number / bool -> usable text form rather than a dropped arg.
    REQUIRE(coerce_string(nlohmann::json{{"s", 3}},    "s") == "3");
    REQUIRE(coerce_string(nlohmann::json{{"s", true}}, "s") == "true");
    // Array / object -> fallback (no throw).
    REQUIRE(coerce_string(nlohmann::json{{"s", nlohmann::json::array({1})}}, "s", "fb") == "fb");
}

TEST_CASE("search_ast: null optional string arg does not throw (the live failure)",
          "[tool_arg_validation][coerce_string]")
{
    auto tmp = make_tmp();
    write_file(tmp, "a.cpp", "class A : public Base {};\n");
    locus::test::FakeWorkspaceServices ws{tmp};
    locus::SearchAstTool tool;

    // The exact shape from the gemma-4-e4b integration run: the model sent a
    // null for an optional string arg (capture / path_glob). Before coerce_string
    // this threw json type_error.302 and surfaced as a confusing tool error.
    locus::ToolCall call{"c1", "search_ast",
        {{"language", "cpp"}, {"query", "(class_specifier) @c"},
         {"capture", nullptr}, {"path_glob", nullptr}}};

    REQUIRE_NOTHROW(tool.preview(call));
    locus::ToolResult r;
    REQUIRE_NOTHROW(r = tool.execute(call, ws));
    // We don't assert on match content (AST availability varies in the test
    // workspace); the point is it does not THROW on the null args.

    fs::remove_all(tmp);
}

TEST_CASE("edit_file: stringified edits array applies and preview does not throw",
          "[tool_arg_validation][coerce_json_array]")
{
    auto tmp = make_tmp();
    write_file(tmp, "code.cpp", "int x = 1;\n");
    locus::test::FakeWorkspaceServices ws{tmp};

    // edit_file requires the file to have been read first -- run read_file so
    // the ReadTracker is primed with the exact canonical path execute() checks.
    {
        locus::ReadFileTool reader;
        locus::ToolCall rc{"r", "read_file", {{"path", "code.cpp"}}};
        REQUIRE(reader.execute(rc, ws).success);
    }

    locus::EditFileTool tool;
    // The model double-encoded the edits array as a string.
    locus::ToolCall call{"c1", "edit_file",
        {{"file_path", "code.cpp"},
         {"edits", "[{\"old_string\":\"int x = 1;\",\"new_string\":\"int x = 2;\"}]"}}};

    // preview() is the UI-thread crash path -- must not throw.
    REQUIRE_NOTHROW(tool.preview(call));

    // execute() must unwrap the string and actually apply the edit.
    locus::ToolResult r;
    REQUIRE_NOTHROW(r = tool.execute(call, ws));
    REQUIRE(r.success);

    std::string got;
    {
        std::ifstream in(tmp / "code.cpp", std::ios::binary);
        got.assign((std::istreambuf_iterator<char>(in)), {});
    }
    REQUIRE_THAT(got, ContainsSubstring("int x = 2;"));

    locus::tools::ReadTracker::instance().clear();
    fs::remove_all(tmp);
}
