#pragma once

#include "tool.h"

#include <filesystem>
#include <string>

namespace locus {
class IWorkspaceServices;
}

namespace locus::tools {

// Resolve a relative path safely within the workspace root.
// Returns empty path if the resolved path escapes the workspace.
std::filesystem::path resolve_path(IWorkspaceServices& ws, const std::string& rel);

std::string make_relative(IWorkspaceServices& ws, const std::filesystem::path& p);

ToolResult error_result(const std::string& msg);

} // namespace locus::tools
