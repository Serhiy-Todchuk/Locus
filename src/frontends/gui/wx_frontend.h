#pragma once

#include "../../core/frontend.h"

#include <wx/wx.h>
#include <wx/event.h>

#include <string>

namespace locus {

// Custom wxEvent IDs for agent thread -> UI thread marshalling.
wxDECLARE_EVENT(EVT_AGENT_TOKEN,         wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_REASONING_TOKEN, wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_TOOL_PENDING,  wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_TOOL_RESULT,   wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_TURN_START,    wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_TURN_COMPLETE, wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_CONTEXT_METER, wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_COMPACTION,    wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_SESSION_RESET, wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_ERROR,         wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_EMBEDDING_PROGRESS, wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_INDEXING_PROGRESS,  wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_ACTIVITY,      wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_ATTACHED_CONTEXT, wxThreadEvent);
// S4.D plan-mode events.
wxDECLARE_EVENT(EVT_AGENT_MODE_CHANGED,      wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_PLAN_PROPOSED,     wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_PLAN_STEP_ADVANCED, wxThreadEvent);
wxDECLARE_EVENT(EVT_AGENT_PLAN_COMPLETED,    wxThreadEvent);
// S4.L auto-commit event.
wxDECLARE_EVENT(EVT_AGENT_AUTO_COMMIT,       wxThreadEvent);
// S4.F generation-progress event (live token counter).
wxDECLARE_EVENT(EVT_AGENT_GEN_PROGRESS,      wxThreadEvent);

// Thread bridge: IFrontend callbacks (fired on the agent thread) are
// marshalled to the wxWidgets main thread via wxQueueEvent + wxThreadEvent.
// The target wxEvtHandler (typically LocusFrame) binds these events.
class WxFrontend : public IFrontend {
public:
    // handler receives all posted events. Must outlive this object.
    explicit WxFrontend(wxEvtHandler* handler);

    void on_turn_start() override;
    void on_token(std::string_view token) override;
    void on_reasoning_token(std::string_view token) override;
    void on_tool_call_pending(const ToolCall& call,
                              const std::string& preview,
                              bool needs_approval,
                              const std::vector<std::string>& safety_warnings) override;
    void on_tool_result(const std::string& call_id,
                        const std::string& display,
                        bool success) override;
    void on_turn_complete() override;
    void on_context_meter(int used_tokens, int limit,
                          int prompt_tokens, int completion_tokens) override;
    void on_compaction_needed(int used_tokens, int limit) override;
    void on_session_reset() override;
    void on_error(const std::string& message) override;
    void on_embedding_progress(int done, int total) override;
    void on_indexing_progress(int done, int total) override;
    void on_activity(const ActivityEvent& event) override;
    void on_attached_context_changed(
        const std::optional<AttachedContext>& ctx) override;
    // S4.D
    void on_mode_changed(AgentMode mode) override;
    void on_plan_proposed(const Plan& plan) override;
    void on_plan_step_advanced(const std::string& plan_id, int step_idx,
                                PlanStep::Status status,
                                const std::string& notes) override;
    void on_plan_completed(const std::string& plan_id, bool success) override;
    // S4.L
    void on_auto_commit(const std::string& short_sha,
                         const std::string& branch,
                         const std::string& subject) override;
    // S4.F
    void on_generation_progress(int chars, int est_tokens) override;

private:
    wxEvtHandler* handler_;
};

} // namespace locus
