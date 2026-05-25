#include "tools/tools.h"

#include <memory>

namespace locus {

void register_builtin_tools(IToolRegistry& registry)
{
    registry.register_tool(std::make_unique<ReadFileTool>());
    registry.register_tool(std::make_unique<WriteFileTool>());
    registry.register_tool(std::make_unique<EditFileTool>());
    registry.register_tool(std::make_unique<DeleteFileTool>());
    registry.register_tool(std::make_unique<ListDirectoryTool>());
    // S3.L: one unified `search` face instead of four separate tools.
    registry.register_tool(std::make_unique<SearchTool>());
    registry.register_tool(std::make_unique<GetFileOutlineTool>());
    registry.register_tool(std::make_unique<RunCommandTool>());
    // S4.I -- long-running shell processes.
    registry.register_tool(std::make_unique<RunCommandBgTool>());
    registry.register_tool(std::make_unique<ReadProcessOutputTool>());
    registry.register_tool(std::make_unique<StopProcessTool>());
    registry.register_tool(std::make_unique<ListProcessesTool>());
    registry.register_tool(std::make_unique<AskUserTool>());
    // S4.D -- plan-mode tools. Visibility is gated by visible_in_mode so they
    // only appear in the manifest when the agent is in plan / execute mode.
    registry.register_tool(std::make_unique<ProposePlanTool>());
    registry.register_tool(std::make_unique<MarkStepDoneTool>());
    // S4.R -- memory bank. Both gate themselves off (`available()=false`)
    // when the workspace has no MemoryStore (memory_enabled=false).
    registry.register_tool(std::make_unique<AddMemoryTool>());
    registry.register_tool(std::make_unique<SearchMemoryTool>());
    // S5.Z task 8 -- regex/substring filter over caller-supplied text.
    registry.register_tool(std::make_unique<FilterOutputTool>());
    // S6.11 -- lazy-manifest meta-tool. Always registered; hides itself from
    // the LLM manifest when `WorkspaceConfig::Agent::lazy_tool_manifest` is false.
    registry.register_tool(std::make_unique<DescribeTool>(registry));
}

} // namespace locus
