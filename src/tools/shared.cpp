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
