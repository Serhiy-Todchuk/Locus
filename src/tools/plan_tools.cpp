#include "tools/plan_tools.h"
#include "tools/shared.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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
    // Count via the same tolerant coercer execute() uses so a string-encoded
    // `steps` arg still shows the right count in the approval preview.
    int n_steps = static_cast<int>(
        tools::coerce_json_array(call.args, "steps").size());

    std::string out = "Plan";
    if (!title.empty()) out += ": " + head(title, 60);
    out += " (" + std::to_string(n_steps) + " step";
    if (n_steps != 1) out += "s";
    out += ")";
    return out;
}

ToolResult ProposePlanTool::execute(const ToolCall& call, IWorkspaceServices& /*ws*/,
                                     const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call,
            {"title", "summary", "steps"}, this))
        return *err;

    json out;
    out["ok"]      = true;
    out["title"]   = call.args.value("title", std::string{});
    out["summary"] = call.args.value("summary", std::string{});

    // Resolve `steps` tolerantly: a real array, or a JSON-string-encoded array
    // (incl. Python-dict-style single-quoted bodies small models emit) that the
    // S6.19 coercer unwraps + repairs. coerce_json_array returns an empty array
    // for an unusable shape, which the empty-check below rejects with a clear
    // message.
    json steps_in = tools::coerce_json_array(call.args, "steps");
    if (steps_in.empty()) {
        ToolResult r;
        r.success = false;
        r.content = error_payload(
            "propose_plan: 'steps' parameter is required and must be a non-empty "
            "array of {description, tools_needed?} objects")
                        .dump();
        r.display = "propose_plan: missing 'steps'";
        return r;
    }

    json steps = json::array();
    int idx = 0;
    for (auto& s : steps_in) {
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

ToolResult MarkStepDoneTool::execute(const ToolCall& call, IWorkspaceServices& /*ws*/,
                                      const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call,
            {"step", "status", "notes"}, this))
        return *err;

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
    // S6.17 Task D -- lowercase enum-shaped args so "DONE" doesn't get rejected.
    std::transform(status.begin(), status.end(), status.begin(),
                   [](unsigned char c) { return std::tolower(c); });
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
