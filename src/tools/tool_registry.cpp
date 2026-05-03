#include "tool_registry.h"

#include <spdlog/spdlog.h>
#include <stdexcept>

namespace locus {

void ToolRegistry::register_tool(std::unique_ptr<ITool> tool)
{
    const std::string tool_name = tool->name();
    if (by_name_.count(tool_name)) {
        throw std::runtime_error("Duplicate tool name: " + tool_name);
    }
    spdlog::trace("ToolRegistry: registered '{}'", tool_name);
    by_name_[tool_name] = tool.get();
    tools_.push_back(std::move(tool));
}

ITool* ToolRegistry::find(const std::string& name) const
{
    auto it = by_name_.find(name);
    return (it != by_name_.end()) ? it->second : nullptr;
}

std::vector<ITool*> ToolRegistry::all() const
{
    std::vector<ITool*> result;
    result.reserve(tools_.size());
    for (auto& t : tools_)
        result.push_back(t.get());
    return result;
}

namespace {

// Build one OpenAI function-calling entry for a tool:
//   { "type": "function", "function": { "name", "description", "parameters" } }
nlohmann::json build_entry(const ITool& t)
{
    nlohmann::json props = nlohmann::json::object();
    nlohmann::json required_list = nlohmann::json::array();

    for (auto& p : t.params()) {
        nlohmann::json prop;
        prop["type"] = p.type;
        prop["description"] = p.description;
        props[p.name] = prop;
        if (p.required)
            required_list.push_back(p.name);
    }

    nlohmann::json parameters;
    parameters["type"] = "object";
    parameters["properties"] = props;
    if (!required_list.empty())
        parameters["required"] = required_list;

    nlohmann::json func;
    func["name"] = t.name();
    func["description"] = t.description();
    func["parameters"] = parameters;

    nlohmann::json entry;
    entry["type"] = "function";
    entry["function"] = func;
    return entry;
}

} // namespace

nlohmann::json ToolRegistry::build_schema_json() const
{
    // Unfiltered: one entry per registered tool. Used by tests and by the
    // system-prompt text builder where gating doesn't apply.
    nlohmann::json arr = nlohmann::json::array();
    for (auto& t : tools_)
        arr.push_back(build_entry(*t));
    return arr;
}

nlohmann::json ToolRegistry::build_schema_json(IWorkspaceServices& ws,
                                               ToolMode mode) const
{
    nlohmann::json arr = nlohmann::json::array();
    for (auto& t : tools_) {
        if (!t->visible_in_mode(mode)) continue;
        if (!t->available(ws))         continue;
        arr.push_back(build_entry(*t));
    }
    return arr;
}

ToolCall ToolRegistry::parse_tool_call(const std::string& id,
                                       const std::string& name,
                                       const std::string& arguments_json)
{
    ToolCall call;
    call.id = id;
    call.tool_name = name;

    if (arguments_json.empty()) {
        call.args = nlohmann::json::object();
    } else {
        try {
            call.args = nlohmann::json::parse(arguments_json);
        } catch (const nlohmann::json::parse_error& e) {
            spdlog::warn("Failed to parse tool call args for '{}': {}", name, e.what());
            call.args = nlohmann::json::object();
        }
    }
    return call;
}

} // namespace locus
