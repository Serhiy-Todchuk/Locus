#pragma once

#include "../frontend.h"

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
                              const std::string& preview) override;
    void on_tool_result(const std::string& call_id,
                        const std::string& display) override;
    void on_turn_complete() override;
    void on_context_meter(int used_tokens, int limit) override;
    void on_compaction_needed(int used_tokens, int limit) override;
    void on_session_reset() override;
    void on_error(const std::string& message) override;
    void on_embedding_progress(int done, int total) override;
    void on_indexing_progress(int done, int total) override;
    void on_activity(const ActivityEvent& event) override;
    void on_attached_context_changed(
        const std::optional<AttachedContext>& ctx) override;

private:
    wxEvtHandler* handler_;
};

} // namespace locus
