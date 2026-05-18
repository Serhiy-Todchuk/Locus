#pragma once

#include "tool.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace locus {

class IToolRegistry;

} // namespace locus

namespace locus::tools {

// S5.S -- named permission presets.
//
// Each preset is a snapshot of the per-tool approval policy map. Selecting a
// named preset writes that snapshot into WorkspaceConfig::tool_approval_policies
// (or, at runtime, sits as an override on the ToolDispatcher). "Custom" is a
// detection-only label that the panel renders when the per-tool table does not
// match any of the named signatures exactly.
//
// The signatures live in one table (preset_signature) so the docs in
// roadmap/M5/S5.S-permission-presets.md and the runtime stay in sync. New
// built-in tools added later inherit their default category by name (see
// builtin_tool_category) and slot in without touching the preset matrix.
enum class PermissionPreset {
    read_only,
    ask_before_edits,
    allow_edits,
    allow_all,
    custom
};

const char* to_string(PermissionPreset p);
const char* display_name(PermissionPreset p);
PermissionPreset preset_from_string(std::string_view s);

// Human-readable description shown under the dropdown / in tooltips.
const char* preset_description(PermissionPreset p);

// Build the per-tool policy map this preset writes into
// WorkspaceConfig::tool_approval_policies.
//
// The map is produced by walking the tool registry and assigning each tool
// the policy for its built-in category. MCP tools are covered by a single
// "mcp:*" entry (per-server overrides are managed in the MCP panel).
// `propose_plan` / `mark_step_done` keep their own default (auto) -- listing
// them per preset would be noise. `ask_user` is always `ask`.
//
// Returns custom -> empty map (custom is detection-only; the caller is
// expected to keep the user's hand-tweaked map unchanged).
std::unordered_map<std::string, ToolApprovalPolicy>
preset_signature(PermissionPreset preset, const IToolRegistry& registry);

// Inspect the per-tool override map + the live tool registry and return which
// named preset (if any) matches exactly. Tools without an override are treated
// as their built-in default (ITool::approval_policy()). A bare-prefix entry
// outside the supported "mcp:*" channel, or per-server "mcp:<name>:*" entries
// that diverge from the wildcard, flips detection to Custom.
PermissionPreset
detect_preset(const std::unordered_map<std::string, ToolApprovalPolicy>& overrides,
              const IToolRegistry& registry);

// List in dropdown order (read_only, ask_before_edits, allow_edits, allow_all,
// custom). Custom is included so it can render as a selectable label when the
// table doesn't match a named preset; choosing it from the dropdown is a no-op
// at the panel layer (no signature to apply).
std::vector<PermissionPreset> all_presets_in_order();

// Cells the matrix actually cares about. Used by the panel for the
// "diverging cells" tooltip and by detect_preset for the comparison loop.
//
// Categories returned by builtin_tool_category:
//   "read"       -- read-only / search / list / outline / read_output / list_processes
//   "edit"       -- write_file / edit_file
//   "delete"     -- delete_file
//   "memory"     -- add_memory
//   "process"    -- stop_process
//   "shell"      -- run_command / run_command_bg
//   "interactive"-- ask_user
//   "plan"       -- propose_plan / mark_step_done
//   "mcp"        -- mcp:<server>:<tool>
//   "other"      -- anything else (treated as the tool's built-in default)
std::string builtin_tool_category(const std::string& tool_name);

// Policy this preset assigns to the given category. Used to build signatures
// and to render the tooltip. Returns the tool's built-in default for "other"
// and "plan" categories (those slots are never preset-controlled).
ToolApprovalPolicy policy_for_category(PermissionPreset preset,
                                       const std::string& category,
                                       ToolApprovalPolicy tool_default);

} // namespace locus::tools
