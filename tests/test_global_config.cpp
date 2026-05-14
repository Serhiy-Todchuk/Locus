#include <catch2/catch_test_macros.hpp>

#include "core/global_config.h"
#include "core/global_paths.h"
#include "core/workspace.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using locus::GlobalConfig;
using locus::WorkspaceConfig;

namespace {

// Hermetic scope: redirects ~/.locus/ (and the legacy global root) to a temp
// dir for the duration of one test. Restores prior values on destruction so
// later tests in the same process see a clean environment.
class GlobalPathsScope {
public:
    explicit GlobalPathsScope(const char* tag)
    {
        root_   = fs::temp_directory_path() / (std::string("locus_test_gcfg_") + tag);
        legacy_ = fs::temp_directory_path() / (std::string("locus_test_gcfg_legacy_") + tag);
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::remove_all(legacy_, ec);
        fs::create_directories(root_);

        prev_global_  = save_env("LOCUS_GLOBAL_DIR");
        prev_legacy_  = save_env("LOCUS_LEGACY_GLOBAL_DIR");
        set_env("LOCUS_GLOBAL_DIR",         root_.string());
        set_env("LOCUS_LEGACY_GLOBAL_DIR",  legacy_.string());
    }
    ~GlobalPathsScope()
    {
        restore_env("LOCUS_GLOBAL_DIR",        prev_global_);
        restore_env("LOCUS_LEGACY_GLOBAL_DIR", prev_legacy_);
        std::error_code ec;
        fs::remove_all(root_,   ec);
        fs::remove_all(legacy_, ec);
    }

    const fs::path& root()   const { return root_;   }
    const fs::path& legacy() const { return legacy_; }

private:
    static std::pair<bool, std::string> save_env(const char* k)
    {
        if (const char* v = std::getenv(k); v) return {true, v};
        return {false, {}};
    }
    static void set_env(const char* k, const std::string& v)
    {
#ifdef _WIN32
        _putenv_s(k, v.c_str());
#else
        ::setenv(k, v.c_str(), 1);
#endif
    }
    static void unset_env(const char* k)
    {
#ifdef _WIN32
        _putenv_s(k, "");
#else
        ::unsetenv(k);
#endif
    }
    static void restore_env(const char* k, const std::pair<bool, std::string>& prev)
    {
        if (prev.first) set_env(k, prev.second);
        else            unset_env(k);
    }

    fs::path root_;
    fs::path legacy_;
    std::pair<bool, std::string> prev_global_;
    std::pair<bool, std::string> prev_legacy_;
};

} // namespace

TEST_CASE("Missing global config returns compiled defaults", "[s5.m][global-config]")
{
    GlobalPathsScope scope("missing");

    auto cfg = locus::load_global_config_or_defaults();

    // Spot-check a handful of fields against the compiled defaults so a future
    // refactor that changes parsing without changing defaults still trips us.
    REQUIRE(cfg.llm_endpoint == "http://127.0.0.1:1234");
    REQUIRE(cfg.llm_max_tokens == 8192);
    REQUIRE(cfg.llm_tool_format == "auto");
    REQUIRE(cfg.respect_gitignore == true);
    REQUIRE(cfg.capabilities.semantic_search == true);
    REQUIRE(cfg.capabilities.background_processes == false);
    REQUIRE(cfg.tool_approval_policies.empty());
}

TEST_CASE("Save then load round-trips most fields", "[s5.m][global-config]")
{
    GlobalPathsScope scope("roundtrip");

    GlobalConfig src;
    src.llm_endpoint      = "http://127.0.0.1:8080";
    src.llm_model         = "gemma-4-e4b";
    src.llm_temperature   = 0.42;
    src.llm_max_tokens    = 16384;
    src.llm_tool_format   = "qwen";
    src.llm_top_p         = 0.8;
    src.llm_top_k         = 20;
    src.exclude_patterns  = {"foo/**", "*.bak"};
    src.git_auto_commit   = true;
    src.git_commit_prefix = "[bot] ";
    src.capabilities.background_processes = true;
    src.capabilities.semantic_search      = false;
    src.tool_approval_policies["read_file"]  = locus::ToolApprovalPolicy::auto_approve;
    src.tool_approval_policies["edit_file"]  = locus::ToolApprovalPolicy::deny;

    REQUIRE(locus::save_global_config(src));
    REQUIRE(fs::exists(scope.root() / "config.json"));

    auto loaded = locus::load_global_config_or_defaults();
    REQUIRE(loaded.llm_endpoint      == src.llm_endpoint);
    REQUIRE(loaded.llm_model         == src.llm_model);
    REQUIRE(loaded.llm_temperature   == src.llm_temperature);
    REQUIRE(loaded.llm_max_tokens    == src.llm_max_tokens);
    REQUIRE(loaded.llm_tool_format   == src.llm_tool_format);
    REQUIRE(loaded.llm_top_p         == src.llm_top_p);
    REQUIRE(loaded.llm_top_k         == src.llm_top_k);
    REQUIRE(loaded.exclude_patterns  == src.exclude_patterns);
    REQUIRE(loaded.git_auto_commit   == src.git_auto_commit);
    REQUIRE(loaded.git_commit_prefix == src.git_commit_prefix);
    REQUIRE(loaded.capabilities.background_processes == true);
    REQUIRE(loaded.capabilities.semantic_search      == false);
    REQUIRE(loaded.tool_approval_policies.at("read_file")
            == locus::ToolApprovalPolicy::auto_approve);
    REQUIRE(loaded.tool_approval_policies.at("edit_file")
            == locus::ToolApprovalPolicy::deny);
}

TEST_CASE("Save strips mcp:* approval entries", "[s5.m][global-config]")
{
    GlobalPathsScope scope("filter_mcp");

    GlobalConfig src;
    src.tool_approval_policies["read_file"]            = locus::ToolApprovalPolicy::auto_approve;
    src.tool_approval_policies["mcp:filesystem:*"]     = locus::ToolApprovalPolicy::auto_approve;
    src.tool_approval_policies["mcp:linear:create"]    = locus::ToolApprovalPolicy::deny;

    REQUIRE(locus::save_global_config(src));

    auto loaded = locus::load_global_config_or_defaults();
    REQUIRE(loaded.tool_approval_policies.count("read_file") == 1);
    REQUIRE(loaded.tool_approval_policies.count("mcp:filesystem:*") == 0);
    REQUIRE(loaded.tool_approval_policies.count("mcp:linear:create") == 0);
}

TEST_CASE("Migration copies legacy resources to new dir", "[s5.m][global-config][migration]")
{
    GlobalPathsScope scope("migrate");

    // Stage legacy files at the legacy root.
    fs::create_directories(scope.legacy());
    fs::create_directories(scope.legacy() / "prompts");
    {
        std::ofstream f(scope.legacy() / "mcp.json");
        f << R"({"mcpServers":{}})";
    }
    {
        std::ofstream f(scope.legacy() / "recent_workspaces.json");
        f << R"(["C:\\some\\path"])";
    }
    {
        std::ofstream f(scope.legacy() / "prompts" / "review.md");
        f << "Review the code.";
    }

    // Pre-migration: new dir is empty.
    REQUIRE(!fs::exists(scope.root() / "mcp.json"));
    REQUIRE(!fs::exists(scope.root() / "prompts" / "review.md"));
    REQUIRE(!fs::exists(scope.root() / "recent_workspaces.json"));

    int migrated = locus::global_paths::migrate_legacy_global_dir_once();
    REQUIRE(migrated == 3);

    // New dir has the copies.
    REQUIRE(fs::exists(scope.root() / "mcp.json"));
    REQUIRE(fs::exists(scope.root() / "prompts" / "review.md"));
    REQUIRE(fs::exists(scope.root() / "recent_workspaces.json"));

    // Legacy dir is left intact for the user to inspect / clean up manually.
    REQUIRE(fs::exists(scope.legacy() / "mcp.json"));

    // Second call is a no-op because targets now exist.
    int second = locus::global_paths::migrate_legacy_global_dir_once();
    REQUIRE(second == 0);
}

TEST_CASE("Workspace first-open seeds from global config", "[s5.m][global-config][workspace]")
{
    GlobalPathsScope scope("seed");

    // Author a non-default global config first.
    GlobalConfig src;
    src.llm_endpoint     = "http://127.0.0.1:9999";
    src.llm_max_tokens   = 24000;
    src.exclude_patterns = {"specifically_seeded/**"};
    REQUIRE(locus::save_global_config(src));

    // Spin up a brand-new workspace dir (no .locus/ yet).
    auto ws_root = fs::temp_directory_path() / "locus_test_gcfg_seed_ws";
    std::error_code ec;
    fs::remove_all(ws_root, ec);
    fs::create_directories(ws_root);

    {
        locus::Workspace ws(ws_root);
        const auto& cfg = ws.config();
        REQUIRE(cfg.llm_endpoint     == "http://127.0.0.1:9999");
        REQUIRE(cfg.llm_max_tokens   == 24000);
        REQUIRE(cfg.exclude_patterns == std::vector<std::string>{"specifically_seeded/**"});
    }

    // The workspace persisted the seeded values, so a second open does NOT
    // re-read the global file -- editing the global mid-test must not affect
    // the already-seeded workspace.
    GlobalConfig changed_global;
    changed_global.llm_endpoint = "http://different.host:1111";
    REQUIRE(locus::save_global_config(changed_global));

    {
        locus::Workspace ws(ws_root);
        const auto& cfg = ws.config();
        REQUIRE(cfg.llm_endpoint == "http://127.0.0.1:9999");
    }

    fs::remove_all(ws_root, ec);
}
