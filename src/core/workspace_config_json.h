#pragma once

#include "core/workspace.h"

#include <nlohmann/json.hpp>

namespace locus {

// JSON <-> WorkspaceConfig round-trip. Extracted from workspace.cpp in S5.M so
// the GlobalConfig (~/.locus/config.json) can reuse the exact same shape and
// field semantics without copy-pasting. Both files store the same fields; the
// global file just acts as a template that seeds new workspaces.
WorkspaceConfig workspace_config_from_json(const nlohmann::json& j);
nlohmann::json  workspace_config_to_json(const WorkspaceConfig& cfg);

} // namespace locus
