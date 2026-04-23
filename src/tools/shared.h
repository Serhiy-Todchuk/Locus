#pragma once

#include "tool.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

namespace locus {
class IWorkspaceServices;
}

namespace locus::tools {

// Resolve a relative path safely within the workspace root.
// Returns empty path if the resolved path escapes the workspace.
std::filesystem::path resolve_path(IWorkspaceServices& ws, const std::string& rel);

std::string make_relative(IWorkspaceServices& ws, const std::filesystem::path& p);

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
