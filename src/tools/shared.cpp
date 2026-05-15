#include "tools/shared.h"

#include "core/workspace_services.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#  include <cwctype>
#endif

namespace locus::tools {

namespace fs = std::filesystem;

namespace {

// Case-insensitive on Windows, case-sensitive elsewhere. Matches NTFS default
// and the rest of the filesystem layer's effective behaviour. UNC and drive
// letters are part of the first component so they're covered.
bool components_equal(const fs::path& a, const fs::path& b)
{
#if defined(_WIN32)
    const auto& sa = a.native();
    const auto& sb = b.native();
    if (sa.size() != sb.size()) return false;
    for (size_t i = 0; i < sa.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(sa[i]))
            != std::towlower(static_cast<wint_t>(sb[i])))
            return false;
    }
    return true;
#else
    return a.native() == b.native();
#endif
}

} // namespace

bool is_inside(const fs::path& candidate, const fs::path& root)
{
    fs::path c = candidate.lexically_normal();
    fs::path r = root.lexically_normal();

    // Drop trailing empty component that lexically_normal leaves on paths
    // ending in a separator ("D:/foo/" -> components {"D:", "/", "foo", ""}).
    auto strip_trailing_empty = [](fs::path& p) {
        if (!p.empty() && p.filename().empty())
            p = p.parent_path();
    };
    strip_trailing_empty(c);
    strip_trailing_empty(r);

    auto cit = c.begin();
    auto cend = c.end();
    auto rit = r.begin();
    auto rend = r.end();

    while (rit != rend) {
        if (cit == cend) return false;          // root deeper than candidate
        if (!components_equal(*rit, *cit)) return false;
        ++rit;
        ++cit;
    }
    // Either equal paths (cit == cend) or candidate is strictly deeper.
    return true;
}

fs::path resolve_path(IWorkspaceServices& ws, const std::string& rel)
{
    std::error_code ec;
    fs::path full = fs::canonical(ws.root() / rel, ec);
    if (ec) {
        // canonical failed (file may not exist yet) — try weakly_canonical
        full = fs::weakly_canonical(ws.root() / rel, ec);
        if (ec) return {};
    }

    fs::path root = fs::canonical(ws.root(), ec);
    if (ec) root = ws.root();

    if (!is_inside(full, root))
        return {};   // path traversal attempt

    return full;
}

std::string write_atomic(const fs::path& target, const std::string& content)
{
    fs::path tmp = target;
    tmp += ".locus-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return "cannot open temp file for write: " + tmp.string();
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out.good())
            return "write failed on temp file: " + tmp.string();
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        // Fallback: rename can fail cross-device / with readers holding handles.
        // Copy + remove tmp is the safety net.
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) return "rename/copy to target failed: " + ec.message();
    }
    return {};
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
