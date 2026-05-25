#include <catch2/catch_test_macros.hpp>

#include "core/workspace.h"
#include "tools/tool.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using locus::WorkspaceConfig;

namespace {

fs::path tmp_root(const char* tag)
{
    auto p = fs::temp_directory_path() / (std::string("locus_test_caps_") + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

// Build a workspace + the built-in tool registry on top of it. Returns the
// workspace by unique_ptr so the caller can mutate config() and feed back
// into a filtered manifest build.
struct WsWithTools {
    std::unique_ptr<locus::Workspace>    ws;
    std::unique_ptr<locus::ToolRegistry> tools;
};

WsWithTools make_ws(const fs::path& root)
{
    WsWithTools out;
    out.ws    = std::make_unique<locus::Workspace>(root);
    // Workspace ctor seeds from the developer's ~/.locus/config.json (the
    // global template), which may flip a capability off and break tests
    // that assume documented defaults. Snap back to the documented values
    // here -- each test still flips individual flags as needed afterwards.
    auto& caps = out.ws->config().capabilities;
    caps.background_processes = false;
    caps.semantic_search      = true;
    caps.code_aware_search    = true;
    caps.memory_bank          = true;
    caps.web_retrieval        = false;
    out.tools = std::make_unique<locus::ToolRegistry>();
    locus::register_builtin_tools(*out.tools);
    return out;
}

bool manifest_has_tool(const nlohmann::json& schema, const std::string& name)
{
    for (auto& entry : schema) {
        if (entry.value("type", "") != "function") continue;
        if (entry["function"].value("name", "") == name) return true;
    }
    return false;
}

std::string manifest_description(const nlohmann::json& schema, const std::string& name)
{
    for (auto& entry : schema) {
        if (entry.value("type", "") != "function") continue;
        if (entry["function"].value("name", "") == name)
            return entry["function"].value("description", "");
    }
    return "";
}

} // namespace

TEST_CASE("Capabilities: default values match the stage doc", "[s5.a][capabilities]")
{
    WorkspaceConfig cfg;
    REQUIRE(cfg.capabilities.background_processes == false);
    REQUIRE(cfg.capabilities.semantic_search      == true);
    REQUIRE(cfg.capabilities.code_aware_search    == true);
    REQUIRE(cfg.capabilities.memory_bank          == true);
    REQUIRE(cfg.capabilities.web_retrieval        == false);
}

TEST_CASE("Capabilities: token-estimate constants exist and are positive",
          "[s5.a][capabilities]")
{
    using namespace locus::capability_token_estimates;
    REQUIRE(k_background_processes > 0);
    REQUIRE(k_semantic_search      > 0);
    REQUIRE(k_code_aware_search    > 0);
    REQUIRE(k_memory_bank          > 0);
    REQUIRE(k_web_retrieval        > 0);
}

TEST_CASE("Capabilities: background_processes off hides bg-process tools",
          "[s5.a][capabilities][manifest]")
{
    auto root = tmp_root("bg_off");
    auto kit  = make_ws(root);

    // Default: bg off.
    auto schema = kit.tools->build_schema_json(*kit.ws, locus::ToolMode::agent);
    REQUIRE_FALSE(manifest_has_tool(schema, "run_command_bg"));
    REQUIRE_FALSE(manifest_has_tool(schema, "read_process_output"));
    REQUIRE_FALSE(manifest_has_tool(schema, "stop_process"));
    REQUIRE_FALSE(manifest_has_tool(schema, "list_processes"));

    // Synchronous run_command must remain available regardless.
    REQUIRE(manifest_has_tool(schema, "run_command"));

    // Flip it on -- now they appear.
    kit.ws->config().capabilities.background_processes = true;
    schema = kit.tools->build_schema_json(*kit.ws, locus::ToolMode::agent);
    REQUIRE(manifest_has_tool(schema, "run_command_bg"));
    REQUIRE(manifest_has_tool(schema, "read_process_output"));
    REQUIRE(manifest_has_tool(schema, "stop_process"));
    REQUIRE(manifest_has_tool(schema, "list_processes"));
}

TEST_CASE("Capabilities: code_aware_search off hides get_file_outline",
          "[s5.a][capabilities][manifest]")
{
    auto root = tmp_root("code_off");
    auto kit  = make_ws(root);

    // Default: code-aware on, outline visible.
    auto schema = kit.tools->build_schema_json(*kit.ws, locus::ToolMode::agent);
    REQUIRE(manifest_has_tool(schema, "get_file_outline"));

    kit.ws->config().capabilities.code_aware_search = false;
    schema = kit.tools->build_schema_json(*kit.ws, locus::ToolMode::agent);
    REQUIRE_FALSE(manifest_has_tool(schema, "get_file_outline"));
}

TEST_CASE("Capabilities: SearchTool description prunes mode list with toggles",
          "[s5.a][capabilities][search]")
{
    auto root = tmp_root("search_modes");
    auto kit  = make_ws(root);

    auto schema = kit.tools->build_schema_json(*kit.ws, locus::ToolMode::agent);
    std::string desc = manifest_description(schema, "search");
    REQUIRE_FALSE(desc.empty());
    // All four optional modes mentioned by default.
    REQUIRE(desc.find("symbols")  != std::string::npos);
    REQUIRE(desc.find("ast")      != std::string::npos);
    REQUIRE(desc.find("semantic") != std::string::npos);
    REQUIRE(desc.find("hybrid")   != std::string::npos);

    // Turn code-aware off -> symbols + ast drop.
    kit.ws->config().capabilities.code_aware_search = false;
    schema = kit.tools->build_schema_json(*kit.ws, locus::ToolMode::agent);
    desc = manifest_description(schema, "search");
    REQUIRE(desc.find("symbols")  == std::string::npos);
    REQUIRE(desc.find("ast")      == std::string::npos);
    REQUIRE(desc.find("semantic") != std::string::npos);
    REQUIRE(desc.find("hybrid")   != std::string::npos);

    // Turn semantic off too -> only text + regex left in the mode list.
    kit.ws->config().capabilities.semantic_search = false;
    schema = kit.tools->build_schema_json(*kit.ws, locus::ToolMode::agent);
    desc = manifest_description(schema, "search");
    REQUIRE(desc.find("symbols")  == std::string::npos);
    REQUIRE(desc.find("ast")      == std::string::npos);
    REQUIRE(desc.find("semantic") == std::string::npos);
    REQUIRE(desc.find("hybrid")   == std::string::npos);
    REQUIRE(desc.find("text")     != std::string::npos);
    REQUIRE(desc.find("regex")    != std::string::npos);
}

TEST_CASE("Capabilities: round-trip through config.json preserves values",
          "[s5.a][capabilities][config]")
{
    auto root = tmp_root("roundtrip");

    // Open + mutate + save + close.
    {
        locus::Workspace ws(root);
        ws.config().capabilities.background_processes = true;
        ws.config().capabilities.semantic_search      = false;
        ws.config().capabilities.code_aware_search    = false;
        ws.config().capabilities.memory_bank          = false;
        ws.config().capabilities.web_retrieval        = true;
        ws.save_config();
    }

    // Re-open and inspect.
    {
        locus::Workspace ws(root);
        const auto& c = ws.config().capabilities;
        REQUIRE(c.background_processes == true);
        REQUIRE(c.semantic_search      == false);
        REQUIRE(c.code_aware_search    == false);
        REQUIRE(c.memory_bank          == false);
        REQUIRE(c.web_retrieval        == true);

        // Legacy mirrors propagated.
        REQUIRE(ws.config().index.semantic_search_enabled == false);
        REQUIRE(ws.config().memory.enabled          == false);
    }
}

TEST_CASE("Capabilities: legacy config (no capabilities block) migrates cleanly",
          "[s5.a][capabilities][migration]")
{
    auto root = tmp_root("migrate");

    // Hand-write an old config.json without a "capabilities" block, only
    // the legacy semantic/memory fields. Simulates a workspace opened
    // before S5.A.
    fs::create_directories(root / ".locus");
    {
        std::ofstream f(root / ".locus" / "config.json");
        f << R"({
          "index": { "semantic_search": { "enabled": false } },
          "memory": { "enabled": true }
        })";
    }

    locus::Workspace ws(root);
    const auto& c = ws.config().capabilities;
    // Semantic was off in the legacy file -> capability follows.
    REQUIRE(c.semantic_search == false);
    // Memory was on -> capability follows.
    REQUIRE(c.memory_bank     == true);
    // Other three buckets get the static defaults.
    REQUIRE(c.background_processes == false);
    REQUIRE(c.code_aware_search    == true);
    REQUIRE(c.web_retrieval        == false);
}

TEST_CASE("Capabilities: unfiltered schema is unchanged (description() static)",
          "[s5.a][capabilities][schema]")
{
    auto root = tmp_root("unfiltered");
    auto kit  = make_ws(root);

    // The unfiltered build path doesn't apply description_for; it MUST
    // continue to mention every mode regardless of capability flips so
    // tools and the system-prompt text builder stay deterministic.
    kit.ws->config().capabilities.code_aware_search = false;
    kit.ws->config().capabilities.semantic_search   = false;

    auto schema = kit.tools->build_schema_json();
    std::string desc = manifest_description(schema, "search");
    REQUIRE(desc.find("symbols")  != std::string::npos);
    REQUIRE(desc.find("ast")      != std::string::npos);
    REQUIRE(desc.find("semantic") != std::string::npos);
}

TEST_CASE("Capabilities: web_retrieval toggle has no manifest effect today",
          "[s5.a][capabilities][placeholder]")
{
    auto root = tmp_root("web");
    auto kit  = make_ws(root);

    auto schema_off = kit.tools->build_schema_json(*kit.ws, locus::ToolMode::agent);
    kit.ws->config().capabilities.web_retrieval = true;
    auto schema_on  = kit.tools->build_schema_json(*kit.ws, locus::ToolMode::agent);

    // No tools register against this bucket until M6 ships -- the manifest
    // is identical either way.
    REQUIRE(schema_off.dump() == schema_on.dump());
}
