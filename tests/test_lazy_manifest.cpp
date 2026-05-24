// S6.11 -- lazy tool manifest behaviour tests.
//
// Covers:
// - ToolRegistry::build_schema_json(ws, mode, lazy=true) collapses each
//   non-meta tool's parameters to {type:"object", properties:{},
//   additionalProperties:true} -- preserves description.
// - lazy=true keeps describe_tool's own schema intact (the meta-tool is the
//   one entry whose real shape must survive).
// - lazy=false reproduces the pre-S6.11 behaviour (full schemas everywhere).
// - SystemPromptAssembly's "## Available Tools" section degrades to per-tool
//   one-liners under lazy_manifest=true.
// - SystemPromptAssembly::hash() differs between lazy_manifest=true/false
//   (S4.F prefix-cache invalidation canary).
//
// available(ws) gating of describe_tool is exercised separately by the manual
// verification protocol (the unit test fakes don't carry a Workspace+config).

#include "agent/system_prompt_assembly.h"
#include "core/workspace.h"  // ToolApprovalPolicy enum
#include "tools/tool.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "support/fake_workspace_services.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>

namespace {

using locus::ITool;
using locus::IWorkspaceServices;
using locus::SystemPromptAssembly;
using locus::ToolApprovalPolicy;
using locus::ToolCall;
using locus::ToolFormat;
using locus::ToolMode;
using locus::ToolParam;
using locus::ToolRegistry;
using locus::ToolResult;
using locus::WorkspaceMetadata;

// Test-only tool with a few params. We need a workspace-config-independent
// stand-in for any of the built-ins, since FakeWorkspaceServices doesn't
// expose a Workspace handle and so describe_tool's available() gate stays
// false in this test fixture.
class FakeTool : public ITool {
public:
    std::string name() const override { return "fake"; }
    std::string description() const override { return "fake tool for tests"; }
    std::vector<ToolParam> params() const override
    {
        return {
            ToolParam{"foo", "string",  "a foo",     true},
            ToolParam{"bar", "integer", "a bar",     false},
            ToolParam{"baz", "boolean", "a baz",     true},
        };
    }
    ToolResult execute(const ToolCall&, IWorkspaceServices&,
                        const std::atomic<bool>*) override
    {
        return {true, "fake-ok", "fake-ok"};
    }
};

}  // namespace

TEST_CASE("lazy_manifest: API tools array collapses non-meta parameters",
          "[s6.11][lazy_manifest]")
{
    ToolRegistry reg;
    reg.register_tool(std::make_unique<FakeTool>());
    locus::test::FakeWorkspaceServices ws("/tmp/ws_test");

    auto full = reg.build_schema_json(ws, ToolMode::agent, /*lazy=*/false);
    auto lazy = reg.build_schema_json(ws, ToolMode::agent, /*lazy=*/true);

    REQUIRE(full.size() == 1);
    REQUIRE(lazy.size() == 1);

    auto& full_props = full[0]["function"]["parameters"]["properties"];
    REQUIRE(full_props.contains("foo"));
    REQUIRE(full_props.contains("bar"));
    REQUIRE(full_props.contains("baz"));

    auto& lazy_params = lazy[0]["function"]["parameters"];
    REQUIRE(lazy_params["type"] == "object");
    REQUIRE(lazy_params["properties"].empty());
    REQUIRE(lazy_params["additionalProperties"] == true);

    // Description preserved in both modes (the model needs the hint even when
    // lazy).
    REQUIRE(full[0]["function"]["description"] == "fake tool for tests");
    REQUIRE(lazy[0]["function"]["description"] == "fake tool for tests");
}

TEST_CASE("lazy_manifest: built-in roster collapses every tool",
          "[s6.11][lazy_manifest]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);
    locus::test::FakeWorkspaceServices ws("/tmp/ws_test");

    auto lazy = reg.build_schema_json(ws, ToolMode::agent, /*lazy=*/true);

    // Every entry should have a permissive parameter shape EXCEPT
    // describe_tool itself. Without a real Workspace, describe_tool's
    // available() returns false so it doesn't appear in this manifest -- so
    // we just verify every visible entry is collapsed.
    for (auto& entry : lazy) {
        auto& params = entry["function"]["parameters"];
        if (entry["function"]["name"] == "describe_tool") {
            // Sanity (describe_tool would keep its real schema).
            REQUIRE(params["properties"].contains("name"));
        } else {
            REQUIRE(params["type"] == "object");
            REQUIRE(params["properties"].empty());
            REQUIRE(params["additionalProperties"] == true);
        }
    }
}

TEST_CASE("lazy_manifest: SystemPromptAssembly tools section degrades to summaries",
          "[s6.11][lazy_manifest]")
{
    ToolRegistry reg;
    reg.register_tool(std::make_unique<FakeTool>());

    WorkspaceMetadata meta;
    meta.root          = "/tmp/ws_test";
    meta.file_count    = 0;
    meta.symbol_count  = 0;
    meta.heading_count = 0;

    auto a_full = SystemPromptAssembly::build("", meta, reg, ToolFormat::Auto, "",
                                              /*lazy_manifest=*/false);
    auto a_lazy = SystemPromptAssembly::build("", meta, reg, ToolFormat::Auto, "",
                                              /*lazy_manifest=*/true);

    // Full prompt mentions every param name in prose.
    REQUIRE(a_full.full_text().find("`foo`") != std::string::npos);
    REQUIRE(a_full.full_text().find("`bar`") != std::string::npos);
    REQUIRE(a_full.full_text().find("`baz`") != std::string::npos);

    // Lazy prompt only mentions the tool name + description, NOT the params.
    REQUIRE(a_lazy.full_text().find("**fake**") != std::string::npos);
    REQUIRE(a_lazy.full_text().find("`foo`") == std::string::npos);
    REQUIRE(a_lazy.full_text().find("`bar`") == std::string::npos);
    REQUIRE(a_lazy.full_text().find("`baz`") == std::string::npos);

    // Lazy prompt advertises describe_tool in the section header so the model
    // knows how to fetch a real schema.
    REQUIRE(a_lazy.full_text().find("describe_tool") != std::string::npos);

    // S4.F cache-invalidation canary: hash MUST differ between the two
    // prompt variants. The two are independently cacheable.
    REQUIRE(a_full.hash() != a_lazy.hash());

    // Lazy prompt should be strictly shorter (no param breakdown).
    REQUIRE(a_lazy.full_text().size() < a_full.full_text().size());
}

TEST_CASE("lazy_manifest: byte-stable across two equivalent builds (S4.F)",
          "[s6.11][lazy_manifest][s4.f]")
{
    ToolRegistry reg;
    reg.register_tool(std::make_unique<FakeTool>());
    WorkspaceMetadata meta;
    meta.root          = "/tmp/ws_test";
    meta.file_count    = 0;
    meta.symbol_count  = 0;
    meta.heading_count = 0;

    auto a = SystemPromptAssembly::build("", meta, reg, ToolFormat::Auto, "",
                                         /*lazy_manifest=*/true);
    auto b = SystemPromptAssembly::build("", meta, reg, ToolFormat::Auto, "",
                                         /*lazy_manifest=*/true);
    REQUIRE(a.full_text() == b.full_text());
    REQUIRE(a.hash() == b.hash());
}
