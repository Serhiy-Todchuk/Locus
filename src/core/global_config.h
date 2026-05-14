#pragma once

#include "core/workspace.h"

namespace locus {

// S5.M -- per-user global Locus config. Lives at `~/.locus/config.json` (see
// `global_paths::config_path()`). Acts as a one-time template when a new
// workspace is first opened: `Workspace::load_config` seeds an empty
// `<workspace>/.locus/config.json` from this file and never reads it again.
//
// Same shape as `WorkspaceConfig` so JSON round-trips through the shared
// helpers in workspace_config_json.h. We deliberately reuse the type rather
// than introducing a parallel struct -- the two files describe the same
// surface (most fields are identical defaults; a handful of fields like
// `tool_approval_policies` carry per-workspace context that wouldn't make
// sense globally, and those are filtered at save time below).
using GlobalConfig = WorkspaceConfig;

// Load `~/.locus/config.json`, returning compiled defaults when the file
// is missing or unparseable. Never throws.
GlobalConfig load_global_config_or_defaults();

// Persist a config to `~/.locus/config.json`. Returns true on success.
//
// Filters out `tool_approvals` keys starting with `mcp:` -- those reference
// workspace-specific MCP server names and don't belong in the global
// template (the rationale lives in the S5.M plan). Built-in tool keys
// (`read_file`, `edit_file`, ...) are kept.
bool save_global_config(const GlobalConfig& cfg);

} // namespace locus
