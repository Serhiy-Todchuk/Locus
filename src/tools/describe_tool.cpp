#include "tools/describe_tool.h"

#include "core/workspace.h"
#include "core/workspace_services.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>

namespace locus {

std::string DescribeTool::name() const
{
    return "describe_tool";
}

std::string DescribeTool::description() const
{
    // Kept terse on purpose -- this lives in every lazy-manifest manifest.
    return "Return the full JSON parameter schema for another tool by name. "
           "Call before using any unfamiliar tool when in lazy-manifest mode.";
}

std::vector<ToolParam> DescribeTool::params() const
{
    return {
        ToolParam{"name", "string", "Tool name to describe.", /*required*/ true}
    };
}

ToolApprovalPolicy DescribeTool::approval_policy() const
{
    // Read-only introspection; safe to auto-run.
    return ToolApprovalPolicy::auto_approve;
}

bool DescribeTool::available(IWorkspaceServices& ws) const
{
    if (auto* w = ws.workspace())
        return w->config().lazy_tool_manifest;
    // No workspace handle (test harnesses) -- hide rather than expose to keep
    // the manifest tight by default.
    return false;
}

namespace {

// Render the same JSON shape `ToolRegistry::build_entry` produces for one
// tool, without going through the registry's filtered-manifest path. Keeps
// the output identical to what the model would have seen with lazy_manifest
// off, so the model can rely on a single schema shape.
nlohmann::json full_schema_entry_for(const ITool& t)
{
    nlohmann::json parameters = t.parameters_schema();
    if (parameters.is_object() && !parameters.empty()) {
        if (!parameters.contains("type"))
            parameters["type"] = "object";
    } else {
        nlohmann::json props = nlohmann::json::object();
        nlohmann::json required_list = nlohmann::json::array();
        for (auto& p : t.params()) {
            nlohmann::json prop;
            prop["type"]        = p.type;
            prop["description"] = p.description;
            props[p.name]       = prop;
            if (p.required)
                required_list.push_back(p.name);
        }
        parameters = nlohmann::json::object();
        parameters["type"]       = "object";
        parameters["properties"] = props;
        if (!required_list.empty())
            parameters["required"] = required_list;
    }

    nlohmann::json func;
    func["name"]        = t.name();
    func["description"] = t.description();
    func["parameters"]  = parameters;

    nlohmann::json entry;
    entry["type"]     = "function";
    entry["function"] = func;
    return entry;
}

// Cheap closest-name suggestion (Levenshtein distance, bounded). Used both
// here and -- as a public helper -- by ToolDispatcher when reporting an
// unknown tool. Kept local for now; promote to shared.h if a third caller
// shows up.
int levenshtein(const std::string& a, const std::string& b)
{
    const std::size_t m = a.size();
    const std::size_t n = b.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (std::size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<int>(i);
    for (std::size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= m; ++i) {
        for (std::size_t j = 1; j <= n; ++j) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1,
                                 dp[i][j - 1] + 1,
                                 dp[i - 1][j - 1] + cost});
        }
    }
    return dp[m][n];
}

std::string closest_name(const std::string& target,
                          const std::vector<ITool*>& candidates)
{
    std::string best;
    int best_d = std::numeric_limits<int>::max();
    for (auto* t : candidates) {
        int d = levenshtein(target, t->name());
        if (d < best_d) { best_d = d; best = t->name(); }
    }
    return best;
}

} // namespace

ToolResult DescribeTool::execute(const ToolCall& call,
                                 IWorkspaceServices& ws,
                                 const std::atomic<bool>* /*cancel_flag*/)
{
    ToolResult r;

    std::string requested;
    if (call.args.is_object() && call.args.contains("name") && call.args["name"].is_string())
        requested = call.args["name"].get<std::string>();

    if (requested.empty()) {
        r.success = false;
        r.content = "describe_tool: required argument 'name' missing or not a string.";
        r.display = r.content;
        return r;
    }

    ITool* target = registry_.find(requested);
    if (!target) {
        auto all = registry_.all();
        std::string suggestion = closest_name(requested, all);

        std::ostringstream names;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (i) names << ", ";
            names << all[i]->name();
        }

        std::ostringstream msg;
        msg << "describe_tool: tool '" << requested << "' is not registered.";
        if (!suggestion.empty())
            msg << " Did you mean '" << suggestion << "'?";
        msg << "\nAvailable tools: " << names.str();

        r.success = false;
        r.content = msg.str();
        r.display = r.content;
        return r;
    }

    nlohmann::json entry = full_schema_entry_for(*target);
    r.success = true;
    r.content = entry.dump(2);
    r.display = r.content;
    return r;
}

} // namespace locus
