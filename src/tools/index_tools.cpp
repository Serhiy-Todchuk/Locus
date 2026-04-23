#include "tools/index_tools.h"
#include "tools/shared.h"

#include "core/workspace_services.h"
#include "index_query.h"

#include <spdlog/spdlog.h>

#include <sstream>
#include <string>

namespace locus {

using tools::error_result;

// -- ListDirectoryTool ------------------------------------------------------

ToolResult ListDirectoryTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string path = call.args.value("path", "");
    int depth = call.args.value("depth", 0);

    // Normalize: "." means root, same as empty.
    if (path == "." || path == "./") path.clear();

    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    auto entries = idx->list_directory(path, depth);

    std::ostringstream content;
    std::string display_path = path.empty() ? "." : path;
    content << "[" << display_path << "] " << entries.size() << " entries\n";
    for (auto& e : entries) {
        if (e.is_directory) {
            content << "  " << e.path << "/\n";
        } else {
            content << "  " << e.path;
            if (e.size_bytes > 0)
                content << "  (" << e.size_bytes << " bytes)";
            if (!e.language.empty())
                content << "  [" << e.language << "]";
            content << "\n";
        }
    }

    std::string result = content.str();
    spdlog::trace("list_directory: {} ({} entries)", display_path, entries.size());
    return {true, result, result};
}

// -- GetFileOutlineTool -----------------------------------------------------

ToolResult GetFileOutlineTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string path = call.args.value("path", "");

    if (path.empty())
        return error_result("Error: 'path' parameter is required");

    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    auto entries = idx->get_file_outline(path);

    std::ostringstream content;
    content << "[" << path << "] outline (" << entries.size() << " entries)\n";
    for (auto& e : entries) {
        if (e.type == OutlineEntry::Heading) {
            for (int i = 0; i < e.level; ++i) content << "#";
            content << " " << e.text << "  (line " << e.line << ")\n";
        } else {
            content << "  " << e.kind << " " << e.text;
            if (!e.signature.empty()) content << e.signature;
            content << "  (line " << e.line << ")\n";
        }
    }

    std::string result = content.str();
    spdlog::trace("get_file_outline: {} ({} entries)", path, entries.size());
    return {true, result, result};
}

} // namespace locus
