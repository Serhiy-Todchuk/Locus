#pragma once

#include "tool.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace locus {
class IWorkspaceServices;
}

namespace locus::tools {

// Resolve the effective approval policy for `tool_name`, given the tool's
// built-in default and the per-workspace override map.
//
// Lookup order (first hit wins):
//   1. Exact override match by tool name.
//   2. Prefix override with a ":*" suffix (e.g. "mcp:fs:*" matches
//      "mcp:fs:read_file"). Multiple prefix entries are checked in iteration
//      order -- callers shouldn't define overlapping prefixes.
//   3. Tool's built-in default (`tool_default`).
//
// A bare "*" wildcard is intentionally NOT supported: blanket auto-approve
// for everything would defeat the entire approval gate; if a user wants it
// they have to set it per built-in tool. The ":*" form is the
// per-MCP-server trust toggle added in S4.G phase 2.
ToolApprovalPolicy resolve_approval_policy(
    const std::string& tool_name,
    ToolApprovalPolicy tool_default,
    const std::unordered_map<std::string, ToolApprovalPolicy>& overrides);


// Resolve a relative path safely within the workspace root.
// Returns empty path if the resolved path escapes the workspace.
std::filesystem::path resolve_path(IWorkspaceServices& ws, const std::string& rel);

std::string make_relative(IWorkspaceServices& ws, const std::filesystem::path& p);

// Component-aware workspace containment check (S5.O). True iff `candidate`
// equals `root` or sits strictly inside it; component-walk avoids the
// "/foo" vs "/foo-bar" prefix false positive that a raw string compare
// produces. On Windows the comparison is case-insensitive (NTFS default);
// elsewhere it's exact. Both paths are lexically_normal'd first so trailing
// slashes and "." segments don't shift the component count.
bool is_inside(const std::filesystem::path& candidate,
               const std::filesystem::path& root);

// Atomic write: write to <target>.locus-tmp then rename over the target.
// Keeps the file intact if the process is killed mid-write. Falls back to
// copy_file when rename fails (different volume, reader holding a handle).
// Returns an empty string on success, an error message otherwise.
std::string write_atomic(const std::filesystem::path& target,
                         const std::string& content);

ToolResult error_result(const std::string& msg);

// Process-wide "this file was read by the agent" tracker (S4.A).
// Edit tools require the file to have been read first — mirrors the Claude
// Code pattern that cuts hallucinated edits on local models. The set only
// grows; canonical paths mean workspace switches don't collide.
class ReadTracker {
public:
    static ReadTracker& instance();

    void mark_read(const std::filesystem::path& canonical_path);
    bool was_read(const std::filesystem::path& canonical_path) const;
    void clear();  // tests

private:
    mutable std::mutex              mu_;
    std::unordered_set<std::string> seen_;
};

} // namespace locus::tools
