#include "tools/shared.h"

#include "core/workspace_services.h"

namespace locus::tools {

namespace fs = std::filesystem;

fs::path resolve_path(IWorkspaceServices& ws, const std::string& rel)
{
    std::error_code ec;
    fs::path full = fs::canonical(ws.root() / rel, ec);
    if (ec) {
        // canonical failed (file may not exist yet) — try weakly_canonical
        full = fs::weakly_canonical(ws.root() / rel, ec);
        if (ec) return {};
    }

    // Ensure the resolved path is inside the workspace root
    auto root_str = ws.root().string();
    auto full_str = full.string();
    if (full_str.size() < root_str.size() ||
        full_str.compare(0, root_str.size(), root_str) != 0) {
        return {};  // path traversal attempt
    }
    return full;
}

std::string make_relative(IWorkspaceServices& ws, const fs::path& p)
{
    return fs::relative(p, ws.root()).string();
}

ToolResult error_result(const std::string& msg)
{
    return {false, msg, msg};
}

ToolApprovalPolicy resolve_approval_policy(
    const std::string& tool_name,
    ToolApprovalPolicy tool_default,
    const std::unordered_map<std::string, ToolApprovalPolicy>& overrides)
{
    auto exact = overrides.find(tool_name);
    if (exact != overrides.end()) return exact->second;

    for (const auto& [key, val] : overrides) {
        // Match keys ending in ":*". The matched prefix is everything up
        // to (and including) the trailing ':'; the tool name must be
        // strictly longer than the prefix so "mcp:fs:*" doesn't match the
        // (impossible but well-defined) "mcp:fs:".
        if (key.size() < 2) continue;
        if (key.compare(key.size() - 2, 2, ":*") != 0) continue;
        std::string prefix(key.data(), key.size() - 1);  // drop the '*', keep ':'
        if (tool_name.size() > prefix.size() &&
            tool_name.compare(0, prefix.size(), prefix) == 0) {
            return val;
        }
    }
    return tool_default;
}

// -- ReadTracker ------------------------------------------------------------

ReadTracker& ReadTracker::instance()
{
    static ReadTracker s_tracker;
    return s_tracker;
}

void ReadTracker::mark_read(const fs::path& canonical_path)
{
    std::lock_guard lock(mu_);
    seen_.insert(canonical_path.string());
}

bool ReadTracker::was_read(const fs::path& canonical_path) const
{
    std::lock_guard lock(mu_);
    return seen_.count(canonical_path.string()) > 0;
}

void ReadTracker::clear()
{
    std::lock_guard lock(mu_);
    seen_.clear();
}

} // namespace locus::tools
