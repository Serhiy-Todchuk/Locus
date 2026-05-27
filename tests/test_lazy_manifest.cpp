// S6.11 + S6.10 Tasks I/J -- lazy tool manifest behaviour tests.
//
// S6.11 baseline covers:
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
// S6.10 Task I adds:
// - ToolRegistry::build_schema_json(ws, mode, lazy=true) renders
//   ITool::short_description() in the description slot (instead of the full
//   description()), for every tool except describe_tool itself.
// - SystemPromptAssembly's prose tools section uses short_description() for
//   each rendered bullet under lazy_manifest=true.
//
// S6.10 Task J adds:
// - SystemPromptAssembly's prose tools section is replaced by a one-line
//   pointer ("see API tools[] array") when lazy_manifest=true AND tool_format
//   is OpenAi or Auto. For QwenXml / ClaudeXml the section still renders the
//   per-tool bullets (those formats don't consume the API tools array).
// - SystemPromptAssembly::hash() differs across the four
//   (lazy x tool_format) combinations so KV cache reflects the change.
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
// false in this test fixture. The long `description()` vs the short
// `short_description()` is intentional -- Task I asserts the lazy path picks
// the short form so the manifest actually shrinks.
class FakeTool : public ITool {
public:
    std::string name() const override { return "fake"; }
    std::string description() const override
    {
        return "fake tool for tests with a deliberately long description so "
               "the lazy-mode short_description swap is observable in the "
               "rendered manifest";
    }
    std::string short_description() const override
    {
        return "fake (short)";
    }
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

// Second FakeTool that does NOT override short_description -- the default
// forwards to description(). Used by the Task I regression-guard test to
// prove that overriding short_description() actually shrinks the manifest.
class FakeToolNoShort : public ITool {
public:
    std::string name() const override { return "fake_no_short"; }
    std::string description() const override
    {
        return "fake tool for tests with a deliberately long description so "
               "the lazy-mode short_description swap is observable in the "
               "rendered manifest";
    }
    std::vector<ToolParam> params() const override
    {
        return {
            ToolParam{"foo", "string",  "a foo",     true},
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

    // S6.10 Task I: full mode renders the full description(); lazy mode swaps
    // it for the curated short_description() one-liner. Both slots are
    // populated -- the model always gets some hint about each tool's purpose.
    const std::string full_desc = full[0]["function"]["description"];
    const std::string lazy_desc = lazy[0]["function"]["description"];
    REQUIRE(full_desc.find("deliberately long description") != std::string::npos);
    REQUIRE(lazy_desc == "fake (short)");
    REQUIRE(lazy_desc.size() < full_desc.size());
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

    // ToolFormat::Qwen so Task J doesn't kick in -- the prose section still
    // renders per-tool bullets (Qwen/Claude formats don't consume the API
    // tools[] array, so the prose section IS the listing). The Task J short
    // form is exercised separately below.
    auto a_full = SystemPromptAssembly::build("", meta, reg, ToolFormat::Qwen, "",
                                              /*lazy_manifest=*/false);
    auto a_lazy = SystemPromptAssembly::build("", meta, reg, ToolFormat::Qwen, "",
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

    // S6.10 Task I: the lazy bullet carries short_description(), not the full
    // description(). Verify both -- the short string is present, the
    // long-description prose phrase is not.
    REQUIRE(a_lazy.full_text().find("fake (short)") != std::string::npos);
    REQUIRE(a_lazy.full_text().find("deliberately long description") == std::string::npos);
    // ... and the full assembly carries the long description prose, not the
    // short form alone.
    REQUIRE(a_full.full_text().find("deliberately long description") != std::string::npos);

    // Lazy prompt advertises describe_tool in the section header so the model
    // knows how to fetch a real schema.
    REQUIRE(a_lazy.full_text().find("describe_tool") != std::string::npos);

    // S4.F cache-invalidation canary: hash MUST differ between the two
    // prompt variants. The two are independently cacheable.
    REQUIRE(a_full.hash() != a_lazy.hash());

    // Lazy prompt should be strictly shorter (no param breakdown + short desc).
    REQUIRE(a_lazy.full_text().size() < a_full.full_text().size());
}

// S6.10 Task I regression guard: a tool that overrides short_description()
// produces a shorter lazy assembly than a tool that doesn't. Without the
// override the lazy path falls back to description() which defeats the purpose.
TEST_CASE("lazy_manifest: short_description override shrinks the prose section",
          "[s6.10][lazy_manifest][task_i]")
{
    WorkspaceMetadata meta;
    meta.root          = "/tmp/ws_test";
    meta.file_count    = 0;
    meta.symbol_count  = 0;
    meta.heading_count = 0;

    ToolRegistry reg_with_short;
    reg_with_short.register_tool(std::make_unique<FakeTool>());
    ToolRegistry reg_without_short;
    reg_without_short.register_tool(std::make_unique<FakeToolNoShort>());

    auto a_short = SystemPromptAssembly::build("", meta, reg_with_short,
                                               ToolFormat::Qwen, "",
                                               /*lazy_manifest=*/true);
    auto a_long  = SystemPromptAssembly::build("", meta, reg_without_short,
                                               ToolFormat::Qwen, "",
                                               /*lazy_manifest=*/true);

    // Both render the bullet, but the overriding variant carries the short
    // string and the fallback carries the long description prose.
    REQUIRE(a_short.full_text().find("fake (short)") != std::string::npos);
    REQUIRE(a_long.full_text().find("deliberately long description") != std::string::npos);

    // Strict-shorter check -- the regression we are guarding against is a
    // future tool author skipping the override and silently giving back the
    // token saving Task I bought us.
    REQUIRE(a_short.full_text().size() < a_long.full_text().size());
}

// S6.10 Task J: lazy_manifest + (OpenAi or Auto) replaces the per-tool prose
// section with a single one-line pointer. For Qwen/Claude the bullets stay.
TEST_CASE("lazy_manifest: Task J skips prose tools section for openai/auto formats",
          "[s6.10][lazy_manifest][task_j]")
{
    ToolRegistry reg;
    reg.register_tool(std::make_unique<FakeTool>());

    WorkspaceMetadata meta;
    meta.root          = "/tmp/ws_test";
    meta.file_count    = 0;
    meta.symbol_count  = 0;
    meta.heading_count = 0;

    auto a_openai = SystemPromptAssembly::build("", meta, reg, ToolFormat::OpenAi, "",
                                                /*lazy_manifest=*/true);
    auto a_auto   = SystemPromptAssembly::build("", meta, reg, ToolFormat::Auto, "",
                                                /*lazy_manifest=*/true);
    auto a_qwen   = SystemPromptAssembly::build("", meta, reg, ToolFormat::Qwen, "",
                                                /*lazy_manifest=*/true);
    auto a_claude = SystemPromptAssembly::build("", meta, reg, ToolFormat::Claude, "",
                                                /*lazy_manifest=*/true);

    // OpenAi + Auto: no per-tool bullet; the one-line pointer carries the
    // describe_tool entry-point name + reference to the API tools[] array.
    for (const auto* a : { &a_openai, &a_auto }) {
        REQUIRE(a->full_text().find("**fake**") == std::string::npos);
        REQUIRE(a->full_text().find("fake (short)") == std::string::npos);
        REQUIRE(a->full_text().find("describe_tool") != std::string::npos);
        REQUIRE(a->full_text().find("tools[]") != std::string::npos);
    }

    // Qwen + Claude: bullet still rendered with the short_description text.
    for (const auto* a : { &a_qwen, &a_claude }) {
        REQUIRE(a->full_text().find("**fake**") != std::string::npos);
        REQUIRE(a->full_text().find("fake (short)") != std::string::npos);
    }

    // Hash differs across all four (lazy x tool_format) combinations. The
    // openai and auto hashes can match each other (they render the same
    // skipped form + the same wire-format addendum for both, since OpenAi /
    // Auto both have an empty format addendum). Qwen and Claude differ
    // because of both the bullet AND the wire-format addendum.
    REQUIRE(a_qwen.hash()   != a_claude.hash());
    REQUIRE(a_qwen.hash()   != a_openai.hash());
    REQUIRE(a_claude.hash() != a_openai.hash());
    REQUIRE(a_qwen.hash()   != a_auto.hash());
    REQUIRE(a_claude.hash() != a_auto.hash());

    // Skipped form should be strictly shorter than the bullet form on the
    // same lazy setting (this is the load-bearing token win for Task J).
    REQUIRE(a_openai.full_text().size() < a_qwen.full_text().size());
    REQUIRE(a_auto.full_text().size()   < a_qwen.full_text().size());
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

// S6.17 Task H -- prompt_cost preset.

#include "core/workspace_config.h"
#include "core/workspace_config_json.h"

TEST_CASE("prompt_cost_to_flags: named presets resolve correctly",
          "[s6.17][prompt_cost]")
{
    using locus::prompt_cost_to_flags;
    auto min  = prompt_cost_to_flags("minimal",  16000, false, "full");
    auto bal  = prompt_cost_to_flags("balanced", 16000, false, "full");
    auto verb = prompt_cost_to_flags("verbose",  16000, false, "full");

    REQUIRE(min.lazy_tool_manifest);
    REQUIRE(min.system_prompt_profile == "minimal");
    REQUIRE(bal.lazy_tool_manifest);
    REQUIRE(bal.system_prompt_profile == "compact");
    REQUIRE_FALSE(verb.lazy_tool_manifest);
    REQUIRE(verb.system_prompt_profile == "full");

    // "default" branches on context size.
    auto def_small  = prompt_cost_to_flags("default",  8000, false, "full");
    auto def_huge   = prompt_cost_to_flags("default", 128000, false, "full");
    REQUIRE(def_small.system_prompt_profile == "compact");   // balanced
    REQUIRE_FALSE(def_huge.lazy_tool_manifest);              // verbose

    // Empty / unknown -> caller's manual flags untouched.
    auto empty_keeps = prompt_cost_to_flags("",        32000, true, "minimal");
    REQUIRE(empty_keeps.lazy_tool_manifest);
    REQUIRE(empty_keeps.system_prompt_profile == "minimal");

    auto bogus = prompt_cost_to_flags("nonsense", 32000, true, "minimal");
    REQUIRE(bogus.system_prompt_profile == "minimal");
}

TEST_CASE("prompt_cost_apply: overrides individual flags when preset set",
          "[s6.17][prompt_cost]")
{
    locus::WorkspaceConfig cfg;
    cfg.llm.context_limit              = 16000;
    cfg.agent.lazy_tool_manifest       = false;
    cfg.agent.system_prompt_profile    = "full";
    cfg.agent.prompt_cost              = "balanced";
    locus::prompt_cost_apply(cfg);
    REQUIRE(cfg.agent.lazy_tool_manifest);
    REQUIRE(cfg.agent.system_prompt_profile == "compact");

    // Empty preset is a no-op (back-compat).
    locus::WorkspaceConfig cfg2;
    cfg2.agent.lazy_tool_manifest    = true;
    cfg2.agent.system_prompt_profile = "minimal";
    cfg2.agent.prompt_cost           = "";
    locus::prompt_cost_apply(cfg2);
    REQUIRE(cfg2.agent.lazy_tool_manifest);
    REQUIRE(cfg2.agent.system_prompt_profile == "minimal");
}

TEST_CASE("prompt_cost: JSON round-trips through workspace_config",
          "[s6.17][prompt_cost]")
{
    locus::WorkspaceConfig cfg;
    cfg.agent.prompt_cost = "minimal";
    cfg.llm.context_limit = 32000;
    auto j  = locus::workspace_config_to_json(cfg);
    auto cb = locus::workspace_config_from_json(j);
    // Note: from_json applies the preset, so the resolved flags reflect
    // "minimal" even though the underlying preset string also round-trips.
    REQUIRE(cb.agent.prompt_cost == "minimal");
    REQUIRE(cb.agent.lazy_tool_manifest);
    REQUIRE(cb.agent.system_prompt_profile == "minimal");
}
