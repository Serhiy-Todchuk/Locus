#include "core/global_paths.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <system_error>

namespace locus::global_paths {

namespace {

fs::path home_dir()
{
#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return fs::path(up);
    // Fallback: piece together HOMEDRIVE + HOMEPATH (rare in modern Windows).
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path  = std::getenv("HOMEPATH");
    if (drive && *drive && path && *path)
        return fs::path(std::string(drive) + path);
    return {};
#else
    if (const char* h = std::getenv("HOME"); h && *h)
        return fs::path(h);
    return {};
#endif
}

// Test-only override roots. Production reads HOME / USERPROFILE / APPDATA
// directly; tests set these env vars to point at hermetic temp dirs.
fs::path env_override(const char* env_name)
{
    if (const char* v = std::getenv(env_name); v && *v) {
        return fs::path(v);
    }
    return {};
}

} // namespace

fs::path global_dir()
{
    fs::path dir;
    if (auto ov = env_override("LOCUS_GLOBAL_DIR"); !ov.empty()) {
        dir = ov;
    } else {
        auto home = home_dir();
        if (home.empty()) return {};
        dir = home / ".locus";
    }

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        spdlog::warn("Cannot create global dir {}: {}", dir.string(), ec.message());
    }
    return dir;
}

fs::path config_path()
{
    auto d = global_dir();
    return d.empty() ? fs::path{} : d / "config.json";
}

fs::path mcp_config_path()
{
    auto d = global_dir();
    return d.empty() ? fs::path{} : d / "mcp.json";
}

fs::path prompts_dir()
{
    auto d = global_dir();
    return d.empty() ? fs::path{} : d / "prompts";
}

fs::path recent_workspaces_path()
{
    auto d = global_dir();
    return d.empty() ? fs::path{} : d / "recent_workspaces.json";
}

// -- Legacy resolvers ---------------------------------------------------------
// Mirror the pre-S5.M lookups exactly so we can locate files written by
// older builds and copy them to the new home. Returns empty path when
// the relevant env var is missing.

// Test override for the legacy root. Tests set LOCUS_LEGACY_GLOBAL_DIR to a
// temp dir; we read mcp.json / prompts / recent_workspaces.json directly off
// it. In production the env var is unset and the platform-specific resolution
// below runs.
namespace {
fs::path legacy_root()
{
    return env_override("LOCUS_LEGACY_GLOBAL_DIR");
}
} // namespace

fs::path legacy_mcp_config_path()
{
    if (auto root = legacy_root(); !root.empty()) return root / "mcp.json";
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return fs::path(appdata) / "Locus" / "mcp.json";
    if (const char* userprofile = std::getenv("USERPROFILE"); userprofile && *userprofile)
        return fs::path(userprofile) / "AppData" / "Roaming" / "Locus" / "mcp.json";
    return {};
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "Locus" / "mcp.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / "Locus" / "mcp.json";
    return {};
#endif
}

fs::path legacy_prompts_dir()
{
    if (auto root = legacy_root(); !root.empty()) return root / "prompts";
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return fs::path(appdata) / "Locus" / "prompts";
    if (const char* userprofile = std::getenv("USERPROFILE"); userprofile && *userprofile)
        return fs::path(userprofile) / "AppData" / "Roaming" / "Locus" / "prompts";
    return {};
#else
    // Pre-S5.M prompts used a lowercase XDG path or ~/.locus/prompts.
    // The ~/.locus/prompts case is already the new path, so migration is
    // a no-op there -- only the XDG branch needs copying.
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "locus" / "prompts";
    return {};
#endif
}

fs::path legacy_recent_workspaces_path()
{
    if (auto root = legacy_root(); !root.empty()) return root / "recent_workspaces.json";
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return fs::path(appdata) / "Locus" / "recent_workspaces.json";
    if (const char* userprofile = std::getenv("USERPROFILE"); userprofile && *userprofile)
        return fs::path(userprofile) / "AppData" / "Roaming" / "Locus" / "recent_workspaces.json";
    return {};
#else
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / "locus" / "recent_workspaces.json";
    return {};
#endif
}

// -- Migration ---------------------------------------------------------------

int migrate_legacy_global_dir_once()
{
    int migrated = 0;

    auto migrate_file = [&](const fs::path& legacy, const fs::path& target) {
        if (legacy.empty() || target.empty()) return;
        if (legacy == target) return;
        std::error_code ec;
        if (fs::exists(target, ec)) return;
        if (!fs::exists(legacy, ec)) return;

        fs::create_directories(target.parent_path(), ec);
        ec.clear();
        fs::copy_file(legacy, target, fs::copy_options::skip_existing, ec);
        if (ec) {
            spdlog::warn("Migration: failed to copy {} -> {}: {}",
                         legacy.string(), target.string(), ec.message());
            return;
        }
        spdlog::info("Migrated {} -> {}", legacy.string(), target.string());
        ++migrated;
    };

    auto migrate_dir = [&](const fs::path& legacy, const fs::path& target) {
        if (legacy.empty() || target.empty()) return;
        if (legacy == target) return;
        std::error_code ec;
        if (fs::exists(target, ec)) return;
        if (!fs::is_directory(legacy, ec)) return;

        fs::create_directories(target.parent_path(), ec);
        ec.clear();
        fs::copy(legacy, target,
                 fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        if (ec) {
            spdlog::warn("Migration: failed to copy {}/ -> {}/: {}",
                         legacy.string(), target.string(), ec.message());
            return;
        }
        spdlog::info("Migrated {}/ -> {}/", legacy.string(), target.string());
        ++migrated;
    };

    migrate_file(legacy_mcp_config_path(),         mcp_config_path());
    migrate_dir (legacy_prompts_dir(),             prompts_dir());
    migrate_file(legacy_recent_workspaces_path(),  recent_workspaces_path());

    if (migrated > 0) {
        spdlog::info("Migrated {} legacy global resource(s) to {}",
                     migrated, global_dir().string());
    }
    return migrated;
}

} // namespace locus::global_paths
