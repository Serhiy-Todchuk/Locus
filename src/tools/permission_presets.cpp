#include "tools/permission_presets.h"

#include "tools/tool.h"

#include <algorithm>

namespace locus::tools {

namespace {

constexpr const char* k_mcp_wildcard = "mcp:*";

bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& s, const char* prefix)
{
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

} // namespace

const char* to_string(PermissionPreset p)
{
    switch (p) {
    case PermissionPreset::read_only:        return "read_only";
    case PermissionPreset::ask_before_edits: return "ask_before_edits";
    case PermissionPreset::allow_edits:      return "allow_edits";
    case PermissionPreset::allow_all:        return "allow_all";
    case PermissionPreset::custom:           return "custom";
    }
    return "custom";
}

const char* display_name(PermissionPreset p)
{
    switch (p) {
    case PermissionPreset::read_only:        return "Read-only";
    case PermissionPreset::ask_before_edits: return "Ask before edits";
    case PermissionPreset::allow_edits:      return "Allow edits";
    case PermissionPreset::allow_all:        return "Allow all";
    case PermissionPreset::custom:           return "Custom";
    }
    return "Custom";
}

PermissionPreset preset_from_string(std::string_view s)
{
    if (s == "read_only")        return PermissionPreset::read_only;
    if (s == "ask_before_edits") return PermissionPreset::ask_before_edits;
    if (s == "allow_edits")      return PermissionPreset::allow_edits;
    if (s == "allow_all")        return PermissionPreset::allow_all;
    return PermissionPreset::custom;
}

const char* preset_description(PermissionPreset p)
{
    switch (p) {
    case PermissionPreset::read_only:
        return "All mutations denied. The agent can read, search, and list -- "
               "nothing more.";
    case PermissionPreset::ask_before_edits:
        return "Every mutation prompts for approval. Conservative default.";
    case PermissionPreset::allow_edits:
        return "File edits auto-approved. Shell, delete_file, and MCP still ask.";
    case PermissionPreset::allow_all:
        return "Files and shell auto-approved. MCP tools still ask -- "
               "elevate per server under Settings -> MCP if you really mean it.";
    case PermissionPreset::custom:
        return "Per-tool overrides do not match any named preset.";
    }
    return "";
}

std::vector<PermissionPreset> all_presets_in_order()
{
    return {
        PermissionPreset::read_only,
        PermissionPreset::ask_before_edits,
        PermissionPreset::allow_edits,
        PermissionPreset::allow_all,
        PermissionPreset::custom,
    };
}

std::string builtin_tool_category(const std::string& name)
{
    // MCP tools are namespaced "mcp:<server>:<tool>".
    if (starts_with(name, "mcp:")) return "mcp";

    if (name == "read_file"
        || name == "search"
        || name == "search_text"
        || name == "search_regex"
        || name == "search_symbols"
        || name == "search_semantic"
        || name == "search_hybrid"
        || name == "list_directory"
        || name == "get_file_outline"
        || name == "read_process_output"
        || name == "list_processes"
        || name == "search_memory")
        return "read";

    if (name == "write_file" || name == "edit_file")
        return "edit";

    if (name == "delete_file")
        return "delete";

    if (name == "add_memory")
        return "memory";

    if (name == "stop_process")
        return "process";

    if (name == "run_command" || name == "run_command_bg")
        return "shell";

    if (name == "ask_user")
        return "interactive";

    if (name == "propose_plan" || name == "mark_step_done")
        return "plan";

    return "other";
}

ToolApprovalPolicy policy_for_category(PermissionPreset preset,
                                       const std::string& category,
                                       ToolApprovalPolicy tool_default)
{
    // Plan and interactive tools are orthogonal to the preset. Read tools are
    // always auto. Returning the tool default for "other" and "plan" keeps
    // unknown / future built-ins doing whatever their author declared.
    if (category == "interactive") return ToolApprovalPolicy::ask;
    if (category == "plan")        return tool_default;
    if (category == "read")        return ToolApprovalPolicy::auto_approve;

    switch (preset) {
    case PermissionPreset::read_only:
        if (category == "edit"   || category == "delete"
            || category == "memory"|| category == "process"
            || category == "shell")
            return ToolApprovalPolicy::deny;
        if (category == "mcp")
            return ToolApprovalPolicy::ask;
        return tool_default;

    case PermissionPreset::ask_before_edits:
        if (category == "edit"   || category == "delete"
            || category == "memory"|| category == "process"
            || category == "shell"|| category == "mcp")
            return ToolApprovalPolicy::ask;
        return tool_default;

    case PermissionPreset::allow_edits:
        if (category == "edit" || category == "memory" || category == "process")
            return ToolApprovalPolicy::auto_approve;
        if (category == "delete" || category == "shell" || category == "mcp")
            return ToolApprovalPolicy::ask;
        return tool_default;

    case PermissionPreset::allow_all:
        if (category == "edit"   || category == "delete"
            || category == "memory"|| category == "process"
            || category == "shell")
            return ToolApprovalPolicy::auto_approve;
        if (category == "mcp")
            return ToolApprovalPolicy::ask;
        return tool_default;

    case PermissionPreset::custom:
        return tool_default;
    }
    return tool_default;
}

std::unordered_map<std::string, ToolApprovalPolicy>
preset_signature(PermissionPreset preset, const IToolRegistry& registry)
{
    std::unordered_map<std::string, ToolApprovalPolicy> out;
    if (preset == PermissionPreset::custom) return out;

    for (ITool* tool : registry.all()) {
        const std::string name = tool->name();
        const std::string cat  = builtin_tool_category(name);
        if (cat == "plan" || cat == "other") continue;            // not preset-controlled
        if (cat == "interactive") {
            // ask_user is always ask. Only record it as an override when the
            // tool's built-in default disagrees; today it agrees, so this stays
            // empty. Keeps the on-disk override map small.
            if (tool->approval_policy() != ToolApprovalPolicy::ask)
                out[name] = ToolApprovalPolicy::ask;
            continue;
        }
        if (cat == "mcp") continue;  // covered by the wildcard below

        ToolApprovalPolicy assigned =
            policy_for_category(preset, cat, tool->approval_policy());
        if (assigned != tool->approval_policy())
            out[name] = assigned;
    }

    // MCP wildcard. All four named presets keep MCP at ask -- the user must
    // explicitly elevate per server via the MCP panel.
    out[k_mcp_wildcard] = ToolApprovalPolicy::ask;
    return out;
}

PermissionPreset
detect_preset(const std::unordered_map<std::string, ToolApprovalPolicy>& overrides,
              const IToolRegistry& registry)
{
    // Snapshot effective policy per tool (override OR default).
    auto effective_for = [&](const std::string& name,
                             ToolApprovalPolicy default_policy) {
        auto it = overrides.find(name);
        return it == overrides.end() ? default_policy : it->second;
    };

    // The MCP wildcard is its own slot. Per-server overrides that diverge
    // from the wildcard force Custom (the user has hand-tuned MCP trust).
    auto mcp_it = overrides.find(k_mcp_wildcard);
    ToolApprovalPolicy mcp_wildcard = mcp_it == overrides.end()
                                          ? ToolApprovalPolicy::ask
                                          : mcp_it->second;
    for (const auto& [key, val] : overrides) {
        if (key == k_mcp_wildcard) continue;
        if (starts_with(key, "mcp:") && ends_with(key, ":*")) {
            if (val != mcp_wildcard) return PermissionPreset::custom;
        }
    }
    // Exact-named "mcp:server:tool" entries also break detection.
    for (const auto& [key, val] : overrides) {
        if (key == k_mcp_wildcard) continue;
        if (starts_with(key, "mcp:") && !ends_with(key, ":*")) {
            if (val != mcp_wildcard) return PermissionPreset::custom;
        }
    }

    for (PermissionPreset preset : {
             PermissionPreset::read_only,
             PermissionPreset::ask_before_edits,
             PermissionPreset::allow_edits,
             PermissionPreset::allow_all}) {

        bool match = true;

        if (mcp_wildcard != ToolApprovalPolicy::ask) {
            // Every named preset puts mcp:* at ask. Anything else is Custom.
            match = false;
            continue;
        }

        for (ITool* tool : registry.all()) {
            const std::string name = tool->name();
            const std::string cat  = builtin_tool_category(name);
            if (cat == "plan" || cat == "other" || cat == "mcp") continue;

            ToolApprovalPolicy want =
                policy_for_category(preset, cat, tool->approval_policy());
            ToolApprovalPolicy have =
                effective_for(name, tool->approval_policy());
            if (want != have) { match = false; break; }
        }
        if (match) return preset;
    }
    return PermissionPreset::custom;
}

} // namespace locus::tools
