#include "tools/tools.h"

#include <memory>

namespace locus {

void register_builtin_tools(IToolRegistry& registry)
{
    registry.register_tool(std::make_unique<ReadFileTool>());
    registry.register_tool(std::make_unique<WriteFileTool>());
    registry.register_tool(std::make_unique<CreateFileTool>());
    registry.register_tool(std::make_unique<DeleteFileTool>());
    registry.register_tool(std::make_unique<ListDirectoryTool>());
    registry.register_tool(std::make_unique<SearchTextTool>());
    registry.register_tool(std::make_unique<SearchSymbolsTool>());
    registry.register_tool(std::make_unique<GetFileOutlineTool>());
    registry.register_tool(std::make_unique<RunCommandTool>());
    registry.register_tool(std::make_unique<AskUserTool>());
    registry.register_tool(std::make_unique<SearchSemanticTool>());
    registry.register_tool(std::make_unique<SearchHybridTool>());
}

} // namespace locus
