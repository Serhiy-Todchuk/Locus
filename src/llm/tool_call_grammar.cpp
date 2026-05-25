#include "llm/tool_call_grammar.h"

namespace locus {

nlohmann::json build_tool_call_union_schema(const std::vector<ToolSchema>& tools)
{
    if (tools.empty()) return nlohmann::json();  // null -- caller skips attach

    nlohmann::json variants = nlohmann::json::array();
    for (const auto& t : tools) {
        nlohmann::json variant;
        variant["type"] = "object";

        nlohmann::json props = nlohmann::json::object();
        props["name"]      = nlohmann::json{{"const", t.name}};

        // The tool's existing parameters schema is the right shape for the
        // `arguments` slot. Some tools supply a fully-formed JSON Schema
        // already (MCP-style); others would have been derived from the flat
        // ToolParam list -- by the time we see them via ToolSchema they are
        // schema-shaped either way (ToolRegistry::build_schema_json normalises
        // both into the same `{type: "object", properties: {...}, ...}` form).
        // Use as-is; default to a permissive object schema when empty.
        if (t.parameters.is_object() && !t.parameters.empty()) {
            props["arguments"] = t.parameters;
        } else {
            props["arguments"] = nlohmann::json{
                {"type", "object"},
                {"additionalProperties", true},
            };
        }

        variant["properties"] = std::move(props);
        variant["required"]   = nlohmann::json::array({"name", "arguments"});
        variants.push_back(std::move(variant));
    }

    nlohmann::json items_schema;
    if (variants.size() == 1) {
        // No union needed -- just the single tool's variant.
        items_schema = variants[0];
    } else {
        items_schema = nlohmann::json{{"oneOf", std::move(variants)}};
    }

    nlohmann::json inner_schema;
    inner_schema["type"]       = "object";
    inner_schema["properties"] = nlohmann::json{
        {"tool_calls", nlohmann::json{
            {"type",     "array"},
            {"minItems", 1},
            {"items",    std::move(items_schema)},
        }}
    };
    inner_schema["required"] = nlohmann::json::array({"tool_calls"});

    nlohmann::json wrapper;
    wrapper["name"]   = "tool_calls";
    wrapper["strict"] = true;
    wrapper["schema"] = std::move(inner_schema);
    return wrapper;
}

bool grammar_should_attach(GrammarMode mode, ToolFormat fmt, std::size_t tool_count)
{
    if (mode == GrammarMode::Off)        return false;
    if (fmt  == ToolFormat::None)        return false;
    if (tool_count == 0)                 return false;
    return true;
}

}  // namespace locus
