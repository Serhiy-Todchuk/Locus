#include "tools/interactive_tools.h"

#include <string>

namespace locus {

std::string AskUserTool::preview(const ToolCall& call) const
{
    return call.args.value("question", "");
}

ToolResult AskUserTool::execute(const ToolCall& call, IWorkspaceServices& /*ws*/)
{
    // The frontend injects the user's response into args["response"] via the
    // modify flow in tool_decision(). If no response was provided (direct
    // approve), return a note that the user approved without answering.
    std::string response = call.args.value("response", "");
    if (response.empty())
        return {true, "[User approved without providing a response]",
                      "[User approved without providing a response]"};

    return {true, response, response};
}

} // namespace locus
