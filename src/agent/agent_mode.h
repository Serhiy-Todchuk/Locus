#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace locus {

// -- AgentMode --------------------------------------------------------------

// Top-level interaction mode the agent is currently in. Selected by the user
// from the chat input area; AgentCore mutates only on the agent thread (via a
// queued request, same pattern as compact_context).
//
//   chat    -- default; tools work as before, no plan involved.
//   plan    -- model is restricted to `propose_plan`; all other tools hidden.
//              The model produces a structured plan; user approves or rejects.
//   execute -- post-approval; full tool catalog plus `mark_step_done` so the
//              agent reports progress as it works through the approved plan.
enum class AgentMode {
    chat,
    plan,
    execute
};

inline const char* to_string(AgentMode m)
{
    switch (m) {
    case AgentMode::chat:    return "chat";
    case AgentMode::plan:    return "plan";
    case AgentMode::execute: return "execute";
    }
    return "chat";
}

inline AgentMode agent_mode_from_string(std::string_view s)
{
    if (s == "plan")    return AgentMode::plan;
    if (s == "execute") return AgentMode::execute;
    return AgentMode::chat;
}

// -- Plan / PlanStep -------------------------------------------------------

struct PlanStep {
    enum class Status {
        pending,
        in_progress,
        done,
        failed
    };

    std::string              description;
    std::vector<std::string> tools_needed;
    Status                   status = Status::pending;
    std::string              notes;   // populated on done/failed by mark_step_done
};

inline const char* to_string(PlanStep::Status s)
{
    switch (s) {
    case PlanStep::Status::pending:     return "pending";
    case PlanStep::Status::in_progress: return "in_progress";
    case PlanStep::Status::done:        return "done";
    case PlanStep::Status::failed:      return "failed";
    }
    return "pending";
}

struct Plan {
    std::string             id;        // monotonic per session ("plan-1", "plan-2", ...)
    std::string             title;
    std::string             summary;
    std::vector<PlanStep>   steps;

    // Convenience: 0-based count of steps in done OR failed state.
    int finished_count() const
    {
        int n = 0;
        for (auto& s : steps) {
            if (s.status == PlanStep::Status::done ||
                s.status == PlanStep::Status::failed) {
                ++n;
            }
        }
        return n;
    }

    bool all_done() const
    {
        if (steps.empty()) return false;
        for (auto& s : steps) {
            if (s.status != PlanStep::Status::done &&
                s.status != PlanStep::Status::failed) {
                return false;
            }
        }
        return true;
    }
};

// -- JSON helpers ----------------------------------------------------------

inline nlohmann::json plan_to_json(const Plan& p)
{
    nlohmann::json j;
    j["id"]      = p.id;
    j["title"]   = p.title;
    j["summary"] = p.summary;
    nlohmann::json arr = nlohmann::json::array();
    for (auto& s : p.steps) {
        nlohmann::json sj;
        sj["description"]  = s.description;
        sj["tools_needed"] = s.tools_needed;
        sj["status"]       = to_string(s.status);
        if (!s.notes.empty()) sj["notes"] = s.notes;
        arr.push_back(std::move(sj));
    }
    j["steps"] = std::move(arr);
    return j;
}

} // namespace locus
