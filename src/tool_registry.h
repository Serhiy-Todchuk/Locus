#pragma once

#include "tool.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace locus {

class ToolRegistry : public IToolRegistry {
public:
    void   register_tool(std::unique_ptr<ITool> tool) override;
    ITool* find(const std::string& name) const override;
    std::vector<ITool*> all() const override;
    nlohmann::json build_schema_json() const override;

    // Parse a ToolCall from an LLM ToolCallRequest (id + name + JSON args string).
    static ToolCall parse_tool_call(const std::string& id,
                                    const std::string& name,
                                    const std::string& arguments_json);

private:
    std::vector<std::unique_ptr<ITool>>          tools_;
    std::unordered_map<std::string, ITool*>      by_name_;
};

} // namespace locus
