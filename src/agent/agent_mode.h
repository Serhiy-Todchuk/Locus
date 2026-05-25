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

// Inverse of plan_to_json -- tolerant of missing fields (used when loading
// pre-S6.x sessions that never carried a plan block). The status string
// is parsed back via the same to_string contract above; unknown values
// default to pending so a hand-edited session file can't crash the load.
inline PlanStep::Status plan_step_status_from_string(std::string_view s)
{
    if (s == "in_progress") return PlanStep::Status::in_progress;
    if (s == "done")        return PlanStep::Status::done;
    if (s == "failed")      return PlanStep::Status::failed;
    return PlanStep::Status::pending;
}

inline Plan plan_from_json(const nlohmann::json& j)
{
    Plan p;
    if (!j.is_object()) return p;
    p.id      = j.value("id",      std::string{});
    p.title   = j.value("title",   std::string{});
    p.summary = j.value("summary", std::string{});
    if (j.contains("steps") && j["steps"].is_array()) {
        for (const auto& sj : j["steps"]) {
            PlanStep s;
            s.description = sj.value("description", std::string{});
            if (sj.contains("tools_needed") && sj["tools_needed"].is_array()) {
                for (const auto& t : sj["tools_needed"]) {
                    if (t.is_string()) s.tools_needed.push_back(t.get<std::string>());
                }
            }
            s.status = plan_step_status_from_string(
                sj.value("status", std::string{"pending"}));
            s.notes  = sj.value("notes", std::string{});
            p.steps.push_back(std::move(s));
        }
    }
    return p;
}

} // namespace locus
