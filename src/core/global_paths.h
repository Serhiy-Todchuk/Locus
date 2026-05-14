#pragma once

#include <filesystem>

namespace locus::global_paths {

namespace fs = std::filesystem;

// S5.M -- single source of truth for the per-user global Locus directory.
// New location is `~/.locus/` on every platform (Windows: C:\Users\<user>\.locus\).
// Matches the convention used by .aider, .claude, .vscode, etc.
//
// Pre-S5.M, three subsystems each resolved their own global path
// (mcp.json, prompts, recent_workspaces.json) under %APPDATA%\Roaming\Locus on
// Windows or scattered XDG / ~/.config locations on Unix. `migrate_legacy_global_dir_once`
// performs a one-shot copy from those legacy locations to the new dir on first launch.

// Returns the global directory. Creates it on first call if missing.
// Empty path on resolution failure (no HOME / USERPROFILE).
fs::path global_dir();

// Path helpers, all rooted at global_dir().
fs::path config_path();              // <global>/config.json
fs::path mcp_config_path();          // <global>/mcp.json
fs::path prompts_dir();              // <global>/prompts
fs::path recent_workspaces_path();   // <global>/recent_workspaces.json

// Legacy paths -- used only by the migration. Each returns the pre-S5.M
// location for one specific resource (paths were inconsistent between
// subsystems, especially on Unix).
fs::path legacy_mcp_config_path();
fs::path legacy_prompts_dir();
fs::path legacy_recent_workspaces_path();

// One-shot migration. For each known resource: if the new path is absent
// and the legacy path exists, copy. Idempotent -- safe to call on every
// startup. Returns the count of resources migrated this call.
int migrate_legacy_global_dir_once();

} // namespace locus::global_paths
