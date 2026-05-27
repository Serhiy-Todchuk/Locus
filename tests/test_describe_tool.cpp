// S6.11 -- describe_tool meta-tool unit tests.
//
// Coverage:
// - Known tool name -> success + JSON schema matches the entry the
//   IToolRegistry would have shipped with the full manifest.
// - Unknown tool name -> success=false with helpful message + closest-match
//   suggestion.
// - Missing/invalid `name` arg -> success=false with usage message.
// - approval_policy() is auto_approve (read-only introspection).
//
// The `available(ws)` gate is workspace-config-driven; covered by the manual
// run protocol and the system-prompt round-trip in test_lazy_manifest.cpp,
// not here.

#include "tools/describe_tool.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "support/fake_workspace_services.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using locus::ToolApprovalPolicy;
using locus::ToolCall;
using locus::ToolRegistry;

}  // namespace

// S6.17 Task J -- describe_tool is the meta-tool the model uses to recover from
// the lazy-manifest collapse. Its OWN ToolParam list must keep a required
// `name` arg + a string type so the lazy-mode collapse can't accidentally
// hollow it out. The actual schema-build-path "describe_tool keeps its full
// schema when lazy=true" guarantee is enforced in test_lazy_manifest.cpp.
TEST_CASE("describe_tool: required 'name' arg pinned via params()",
          "[s6.17][describe_tool][lazy_manifest]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);
    auto* tool = reg.find("describe_tool");
    REQUIRE(tool != nullptr);

    auto params = tool->params();
    bool found_name = false;
    for (const auto& p : params) {
        if (p.name == "name") {
            REQUIRE(p.type == "string");
            REQUIRE(p.required);
            found_name = true;
        }
    }
    REQUIRE(found_name);
}

TEST_CASE("describe_tool: known tool returns full schema", "[s6.11][describe_tool]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);
    locus::test::FakeWorkspaceServices ws("/tmp/ws_test");

    ToolCall call;
    call.id        = "test-1";
    call.tool_name = "describe_tool";
    call.args      = nlohmann::json::object({{"name", "read_file"}});

    auto* tool = reg.find("describe_tool");
    REQUIRE(tool != nullptr);
    auto r = tool->execute(call, ws, nullptr);

    REQUIRE(r.success);
    auto j = nlohmann::json::parse(r.content);
    REQUIRE(j.contains("type"));
    REQUIRE(j["type"] == "function");
    REQUIRE(j["function"]["name"] == "read_file");
    REQUIRE(j["function"]["parameters"].contains("properties"));
    REQUIRE(j["function"]["parameters"]["properties"].contains("path"));
}

TEST_CASE("describe_tool: unknown tool fails with suggestion", "[s6.11][describe_tool]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);
    locus::test::FakeWorkspaceServices ws("/tmp/ws_test");

    ToolCall call;
    call.id        = "test-2";
    call.tool_name = "describe_tool";
    // Close to read_file -- expect that as the suggestion.
    call.args      = nlohmann::json::object({{"name", "readfile"}});

    auto r = reg.find("describe_tool")->execute(call, ws, nullptr);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.content.find("readfile") != std::string::npos);
    REQUIRE(r.content.find("read_file") != std::string::npos);  // suggestion
}

TEST_CASE("describe_tool: missing name arg fails cleanly", "[s6.11][describe_tool]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);
    locus::test::FakeWorkspaceServices ws("/tmp/ws_test");

    ToolCall call;
    call.id        = "test-3";
    call.tool_name = "describe_tool";
    call.args      = nlohmann::json::object();  // no name

    auto r = reg.find("describe_tool")->execute(call, ws, nullptr);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.content.find("name") != std::string::npos);
}

TEST_CASE("describe_tool: approval policy is auto_approve", "[s6.11][describe_tool]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);
    auto* tool = reg.find("describe_tool");
    REQUIRE(tool != nullptr);
    REQUIRE(tool->approval_policy() == ToolApprovalPolicy::auto_approve);
}

TEST_CASE("describe_tool: registered as built-in", "[s6.11][describe_tool]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);
    REQUIRE(reg.find("describe_tool") != nullptr);
}
