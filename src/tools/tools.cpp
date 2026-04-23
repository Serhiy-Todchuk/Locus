#include "tools/tools.h"

#include <memory>

namespace locus {

void register_builtin_tools(IToolRegistry& registry)
{
    registry.register_tool(std::make_unique<ReadFileTool>());
    registry.register_tool(std::make_unique<WriteFileTool>());
    registry.register_tool(std::make_unique<EditFileTool>());
    registry.register_tool(std::make_unique<MultiEditFileTool>());
    registry.register_tool(std::make_unique<CreateFileTool>());
    registry.register_tool(std::make_unique<DeleteFileTool>());
    registry.register_tool(std::make_unique<ListDirectoryTool>());
    // S3.L: one unified `search` face instead of four separate tools.
    registry.register_tool(std::make_unique<SearchTool>());
    registry.register_tool(std::make_unique<GetFileOutlineTool>());
    registry.register_tool(std::make_unique<RunCommandTool>());
    registry.register_tool(std::make_unique<AskUserTool>());
}

} // namespace locus
