#include <catch2/catch_test_macros.hpp>

#include "tools/permission_presets.h"
#include "tools/shared.h"
#include "tools/tool.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"

#include <string>
#include <unordered_map>

using locus::ToolApprovalPolicy;
using locus::ToolRegistry;
using locus::tools::PermissionPreset;
using locus::tools::detect_preset;
using locus::tools::preset_signature;
using locus::tools::policy_for_category;
using locus::tools::builtin_tool_category;
using locus::tools::resolve_approval_policy;

namespace {

ToolRegistry make_builtin_registry()
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);
    return reg;
}

ToolApprovalPolicy effective(const ToolRegistry& reg,
                             const std::unordered_map<std::string,
                                                       ToolApprovalPolicy>& overrides,
                             const std::string& tool_name)
{
    auto* tool = reg.find(tool_name);
    REQUIRE(tool != nullptr);
    return resolve_approval_policy(tool_name, tool->approval_policy(), overrides);
}

} // namespace

TEST_CASE("PermissionPreset: builtin_tool_category covers built-in tools",
          "[s5.s][permission_presets]")
{
    REQUIRE(builtin_tool_category("read_file")       == "read");
    // S6.17 Task G -- six per-mode search_* tools instead of the unified `search`.
    REQUIRE(builtin_tool_category("search_text")     == "read");
    REQUIRE(builtin_tool_category("search_regex")    == "read");
    REQUIRE(builtin_tool_category("search_symbols")  == "read");
    REQUIRE(builtin_tool_category("search_semantic") == "read");
    REQUIRE(builtin_tool_category("search_hybrid")   == "read");
    REQUIRE(builtin_tool_category("search_ast")      == "read");
    REQUIRE(builtin_tool_category("list_directory")  == "read");
    REQUIRE(builtin_tool_category("write_file")      == "edit");
    REQUIRE(builtin_tool_category("edit_file")       == "edit");
    REQUIRE(builtin_tool_category("delete_file")     == "delete");
    REQUIRE(builtin_tool_category("add_memory")      == "memory");
    REQUIRE(builtin_tool_category("stop_process")    == "process");
    REQUIRE(builtin_tool_category("run_command")     == "shell");
    REQUIRE(builtin_tool_category("run_command_bg")  == "shell");
    REQUIRE(builtin_tool_category("ask_user")        == "interactive");
    REQUIRE(builtin_tool_category("propose_plan")    == "plan");
    REQUIRE(builtin_tool_category("mark_step_done")  == "plan");
    REQUIRE(builtin_tool_category("mcp:fs:read")     == "mcp");
    REQUIRE(builtin_tool_category("some_future_tool")== "other");
}

TEST_CASE("PermissionPreset: policy_for_category enforces asymmetric cells",
          "[s5.s][permission_presets]")
{
    // delete stays ask at allow_edits, auto at allow_all.
    REQUIRE(policy_for_category(PermissionPreset::allow_edits, "delete",
                                ToolApprovalPolicy::ask)
            == ToolApprovalPolicy::ask);
    REQUIRE(policy_for_category(PermissionPreset::allow_all, "delete",
                                ToolApprovalPolicy::ask)
            == ToolApprovalPolicy::auto_approve);

    // Shell stays ask at allow_edits, auto at allow_all.
    REQUIRE(policy_for_category(PermissionPreset::allow_edits, "shell",
                                ToolApprovalPolicy::ask)
            == ToolApprovalPolicy::ask);
    REQUIRE(policy_for_category(PermissionPreset::allow_all, "shell",
                                ToolApprovalPolicy::ask)
            == ToolApprovalPolicy::auto_approve);

    // mcp stays ask in all four named presets.
    for (auto p : {PermissionPreset::read_only,
                   PermissionPreset::ask_before_edits,
                   PermissionPreset::allow_edits,
                   PermissionPreset::allow_all}) {
        REQUIRE(policy_for_category(p, "mcp", ToolApprovalPolicy::ask)
                == ToolApprovalPolicy::ask);
    }

    // Read tools are always auto across the named presets.
    for (auto p : {PermissionPreset::read_only,
                   PermissionPreset::ask_before_edits,
                   PermissionPreset::allow_edits,
                   PermissionPreset::allow_all}) {
        REQUIRE(policy_for_category(p, "read", ToolApprovalPolicy::auto_approve)
                == ToolApprovalPolicy::auto_approve);
    }

    // Read-only denies all mutating categories.
    REQUIRE(policy_for_category(PermissionPreset::read_only, "edit",
                                ToolApprovalPolicy::ask)
            == ToolApprovalPolicy::deny);
    REQUIRE(policy_for_category(PermissionPreset::read_only, "delete",
                                ToolApprovalPolicy::ask)
            == ToolApprovalPolicy::deny);
    REQUIRE(policy_for_category(PermissionPreset::read_only, "shell",
                                ToolApprovalPolicy::ask)
            == ToolApprovalPolicy::deny);

    // ask_user always ask.
    REQUIRE(policy_for_category(PermissionPreset::allow_all, "interactive",
                                ToolApprovalPolicy::ask)
            == ToolApprovalPolicy::ask);
    REQUIRE(policy_for_category(PermissionPreset::read_only, "interactive",
                                ToolApprovalPolicy::ask)
            == ToolApprovalPolicy::ask);
}

TEST_CASE("PermissionPreset: detect_preset returns ask_before_edits for empty map",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    std::unordered_map<std::string, ToolApprovalPolicy> empty;
    // With no overrides, the effective per-tool map is each tool's built-in
    // default (write/edit/delete/run_command -> ask; reads -> auto). That
    // matches "Ask before edits" exactly.
    REQUIRE(detect_preset(empty, reg) == PermissionPreset::ask_before_edits);
}

TEST_CASE("PermissionPreset: round-trip preset_signature -> detect_preset",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    for (auto p : {PermissionPreset::read_only,
                   PermissionPreset::ask_before_edits,
                   PermissionPreset::allow_edits,
                   PermissionPreset::allow_all}) {
        auto sig = preset_signature(p, reg);
        INFO("preset=" << locus::tools::to_string(p));
        REQUIRE(detect_preset(sig, reg) == p);
    }
}

TEST_CASE("PermissionPreset: allow_all signature flips shell + delete to auto, mcp:* stays ask",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    auto sig = preset_signature(PermissionPreset::allow_all, reg);
    REQUIRE(effective(reg, sig, "run_command")    == ToolApprovalPolicy::auto_approve);
    REQUIRE(effective(reg, sig, "run_command_bg") == ToolApprovalPolicy::auto_approve);
    REQUIRE(effective(reg, sig, "delete_file")    == ToolApprovalPolicy::auto_approve);
    REQUIRE(effective(reg, sig, "edit_file")      == ToolApprovalPolicy::auto_approve);
    REQUIRE(effective(reg, sig, "write_file")     == ToolApprovalPolicy::auto_approve);
    // mcp:* wildcard sits at ask.
    auto it = sig.find("mcp:*");
    REQUIRE(it != sig.end());
    REQUIRE(it->second == ToolApprovalPolicy::ask);
}

TEST_CASE("PermissionPreset: allow_edits keeps delete + shell at ask",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    auto sig = preset_signature(PermissionPreset::allow_edits, reg);
    REQUIRE(effective(reg, sig, "edit_file")      == ToolApprovalPolicy::auto_approve);
    REQUIRE(effective(reg, sig, "write_file")     == ToolApprovalPolicy::auto_approve);
    REQUIRE(effective(reg, sig, "delete_file")    == ToolApprovalPolicy::ask);
    REQUIRE(effective(reg, sig, "run_command")    == ToolApprovalPolicy::ask);
    REQUIRE(effective(reg, sig, "run_command_bg") == ToolApprovalPolicy::ask);
}

TEST_CASE("PermissionPreset: read_only denies all mutations, keeps reads",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    auto sig = preset_signature(PermissionPreset::read_only, reg);
    REQUIRE(effective(reg, sig, "edit_file")      == ToolApprovalPolicy::deny);
    REQUIRE(effective(reg, sig, "write_file")     == ToolApprovalPolicy::deny);
    REQUIRE(effective(reg, sig, "delete_file")    == ToolApprovalPolicy::deny);
    REQUIRE(effective(reg, sig, "run_command")    == ToolApprovalPolicy::deny);
    REQUIRE(effective(reg, sig, "run_command_bg") == ToolApprovalPolicy::deny);
    REQUIRE(effective(reg, sig, "stop_process")   == ToolApprovalPolicy::deny);
    REQUIRE(effective(reg, sig, "add_memory")     == ToolApprovalPolicy::deny);
    REQUIRE(effective(reg, sig, "read_file")        == ToolApprovalPolicy::auto_approve);
    // S6.17 Task G -- per-mode search_* tools instead of the unified `search`.
    REQUIRE(effective(reg, sig, "search_text")      == ToolApprovalPolicy::auto_approve);
    REQUIRE(effective(reg, sig, "search_regex")     == ToolApprovalPolicy::auto_approve);
    REQUIRE(effective(reg, sig, "search_semantic")  == ToolApprovalPolicy::auto_approve);
    REQUIRE(effective(reg, sig, "list_directory")   == ToolApprovalPolicy::auto_approve);
}

TEST_CASE("PermissionPreset: hand-tuned single cell falls to Custom",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    auto sig = preset_signature(PermissionPreset::allow_edits, reg);
    // Hand-tweak one cell so the map no longer matches any named preset.
    sig["delete_file"] = ToolApprovalPolicy::auto_approve;
    REQUIRE(detect_preset(sig, reg) == PermissionPreset::custom);
}

TEST_CASE("PermissionPreset: per-server MCP elevation diverging from mcp:* is Custom",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    auto sig = preset_signature(PermissionPreset::allow_edits, reg);
    // mcp:* stays ask, but the user marked one server trusted.
    sig["mcp:trusted:*"] = ToolApprovalPolicy::auto_approve;
    REQUIRE(detect_preset(sig, reg) == PermissionPreset::custom);
}

TEST_CASE("PermissionPreset: per-server MCP at same level as mcp:* still matches",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    auto sig = preset_signature(PermissionPreset::allow_edits, reg);
    // Adding a per-server entry at the same level as the wildcard is a no-op
    // from detection's perspective.
    sig["mcp:other:*"] = ToolApprovalPolicy::ask;
    REQUIRE(detect_preset(sig, reg) == PermissionPreset::allow_edits);
}

TEST_CASE("PermissionPreset: mcp wildcard elevated to auto flips to Custom",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    auto sig = preset_signature(PermissionPreset::allow_all, reg);
    // User elevated mcp:* to auto -- now no named preset matches.
    sig["mcp:*"] = ToolApprovalPolicy::auto_approve;
    REQUIRE(detect_preset(sig, reg) == PermissionPreset::custom);
}

TEST_CASE("PermissionPreset: ask_user stays ask in every named preset",
          "[s5.s][permission_presets]")
{
    auto reg = make_builtin_registry();
    for (auto p : {PermissionPreset::read_only,
                   PermissionPreset::ask_before_edits,
                   PermissionPreset::allow_edits,
                   PermissionPreset::allow_all}) {
        auto sig = preset_signature(p, reg);
        INFO("preset=" << locus::tools::to_string(p));
        REQUIRE(effective(reg, sig, "ask_user") == ToolApprovalPolicy::ask);
    }
}

TEST_CASE("PermissionPreset: preset_from_string round-trips",
          "[s5.s][permission_presets]")
{
    using locus::tools::preset_from_string;
    using locus::tools::to_string;
    for (auto p : locus::tools::all_presets_in_order()) {
        if (p == PermissionPreset::custom) continue;  // round-trip skipped for sentinel
        REQUIRE(preset_from_string(to_string(p)) == p);
    }
}
