#include "tools/plan_tools.h"

#include <nlohmann/json.hpp>

#include <string>

namespace locus {

using json = nlohmann::json;

namespace {

// Trim a string to roughly `n` chars on a word boundary; suffixes "...".
std::string head(const std::string& s, size_t n)
{
    if (s.size() <= n) return s;
    auto cut = s.find_last_of(" \t\n", n);
    if (cut == std::string::npos || cut < n / 2) cut = n;
    return s.substr(0, cut) + "...";
}

json error_payload(const std::string& msg)
{
    json j;
    j["ok"]    = false;
    j["error"] = msg;
    return j;
}

} // namespace

// ---- ProposePlanTool ------------------------------------------------------

std::string ProposePlanTool::preview(const ToolCall& call) const
{
    std::string title = call.args.value("title", std::string{});
    int n_steps = 0;
    if (call.args.contains("steps") && call.args["steps"].is_array())
        n_steps = static_cast<int>(call.args["steps"].size());

    std::string out = "Plan";
    if (!title.empty()) out += ": " + head(title, 60);
    out += " (" + std::to_string(n_steps) + " step";
    if (n_steps != 1) out += "s";
    out += ")";
    return out;
}

ToolResult ProposePlanTool::execute(const ToolCall& call, IWorkspaceServices& /*ws*/)
{
    json out;
    out["ok"]      = true;
    out["title"]   = call.args.value("title", std::string{});
    out["summary"] = call.args.value("summary", std::string{});

    if (!call.args.contains("steps") || !call.args["steps"].is_array()) {
        ToolResult r;
        r.success = false;
        r.content = error_payload(
            "propose_plan: 'steps' parameter is required and must be an array")
                        .dump();
        r.display = "propose_plan: missing 'steps'";
        return r;
    }

    json steps = json::array();
    int idx = 0;
    for (auto& s : call.args["steps"]) {
        ++idx;
        if (!s.is_object()) {
            ToolResult r;
            r.success = false;
            r.content = error_payload(
                "propose_plan: step " + std::to_string(idx) +
                " is not an object").dump();
            r.display = "propose_plan: step " + std::to_string(idx) + " malformed";
            return r;
        }
        std::string desc = s.value("description", std::string{});
        if (desc.empty()) {
            ToolResult r;
            r.success = false;
            r.content = error_payload(
                "propose_plan: step " + std::to_string(idx) +
                " is missing 'description'").dump();
            r.display = "propose_plan: step " + std::to_string(idx) +
                        " missing description";
            return r;
        }
        json sj;
        sj["description"] = desc;
        if (s.contains("tools_needed") && s["tools_needed"].is_array())
            sj["tools_needed"] = s["tools_needed"];
        else
            sj["tools_needed"] = json::array();
        steps.push_back(std::move(sj));
    }

    if (steps.empty()) {
        ToolResult r;
        r.success = false;
        r.content = error_payload("propose_plan: 'steps' is empty").dump();
        r.display = "propose_plan: empty plan";
        return r;
    }

    out["steps"] = std::move(steps);

    ToolResult r;
    r.success = true;
    r.content = out.dump();
    r.display = preview(call) +
                " -- proposed; awaiting user approval. "
                "User will Approve (begin execution) or Reject "
                "(re-prompt with corrections).";
    return r;
}

// ---- MarkStepDoneTool -----------------------------------------------------

std::string MarkStepDoneTool::preview(const ToolCall& call) const
{
    int step = call.args.value("step", 0);
    std::string status = call.args.value("status", std::string{"done"});
    std::string notes  = call.args.value("notes",  std::string{});
    std::string out = "Step " + std::to_string(step) + ": " + status;
    if (!notes.empty()) out += " -- " + head(notes, 80);
    return out;
}

ToolResult MarkStepDoneTool::execute(const ToolCall& call, IWorkspaceServices& /*ws*/)
{
    if (!call.args.contains("step")) {
        ToolResult r;
        r.success = false;
        r.content = error_payload("mark_step_done: 'step' parameter is required").dump();
        r.display = "mark_step_done: missing 'step'";
        return r;
    }

    int step = 0;
    try { step = call.args["step"].get<int>(); }
    catch (const json::exception&) {
        ToolResult r;
        r.success = false;
        r.content = error_payload(
            "mark_step_done: 'step' must be an integer").dump();
        r.display = "mark_step_done: 'step' not an integer";
        return r;
    }
    if (step <= 0) {
        ToolResult r;
        r.success = false;
        r.content = error_payload(
            "mark_step_done: 'step' must be a positive 1-based index").dump();
        r.display = "mark_step_done: invalid step index";
        return r;
    }

    std::string status = call.args.value("status", std::string{"done"});
    if (status != "done" && status != "failed") {
        ToolResult r;
        r.success = false;
        r.content = error_payload(
            "mark_step_done: 'status' must be 'done' or 'failed'").dump();
        r.display = "mark_step_done: bad status '" + status + "'";
        return r;
    }

    std::string notes = call.args.value("notes", std::string{});

    json out;
    out["ok"]     = true;
    out["step"]   = step;
    out["status"] = status;
    if (!notes.empty()) out["notes"] = notes;

    ToolResult r;
    r.success = true;
    r.content = out.dump();
    r.display = preview(call);
    return r;
}

} // namespace locus
