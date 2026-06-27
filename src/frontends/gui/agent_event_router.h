#pragma once

#include <wx/event.h>

namespace locus {

class LocusFrame;

// AgentEventRouter -- translates wxThreadEvents posted from the agent thread
// (via WxFrontend) into per-tab UI calls on the owning LocusFrame. Extracted
// from LocusFrame so the frame doesn't carry the 25-method router surface
// directly. Reaches into LocusFrame private state via a friend declaration
// in locus_frame.h.
//
// Lifetime: owned by LocusFrame; destroyed before the frame's Bind table is
// torn down so no late event can fire into a dangling receiver.
//
// Not a wxEvtHandler -- wxEvtHandler::Bind's receiver argument accepts any
// object whose member function the bound pointer-to-member references.
class AgentEventRouter {
public:
    explicit AgentEventRouter(LocusFrame& frame) : frame_(frame) {}

    // 27 handlers; all route by tab_id (evt.GetId()) and update the matching
    // tab's chat / status / footer surfaces.
    void on_agent_turn_start(wxThreadEvent& evt);
    void on_agent_token(wxThreadEvent& evt);
    void on_agent_reasoning_token(wxThreadEvent& evt);
    void on_agent_tool_pending(wxThreadEvent& evt);
    void on_agent_tool_result(wxThreadEvent& evt);
    void on_agent_turn_complete(wxThreadEvent& evt);
    void on_agent_context_meter(wxThreadEvent& evt);
    void on_agent_compaction(wxThreadEvent& evt);
    void on_agent_compaction_archived(wxThreadEvent& evt);
    void on_agent_round_progress(wxThreadEvent& evt);
    void on_agent_llm_retry(wxThreadEvent& evt);
    void on_agent_unverified_success(wxThreadEvent& evt);
    void on_agent_watchdog_tripped(wxThreadEvent& evt);
    void on_agent_watchdog_cleared(wxThreadEvent& evt);
    void on_agent_endpoint_changed(wxThreadEvent& evt);
    void on_agent_session_reset(wxThreadEvent& evt);
    void on_agent_error(wxThreadEvent& evt);
    void on_agent_embedding_progress(wxThreadEvent& evt);
    void on_agent_indexing_progress(wxThreadEvent& evt);
    void on_agent_activity(wxThreadEvent& evt);
    void on_agent_activity_updated(wxThreadEvent& evt);
    void on_agent_attached_context(wxThreadEvent& evt);
    void on_agent_mode_changed(wxThreadEvent& evt);
    void on_agent_plan_proposed(wxThreadEvent& evt);
    void on_agent_plan_step_advanced(wxThreadEvent& evt);
    void on_agent_plan_completed(wxThreadEvent& evt);
    void on_agent_auto_commit(wxThreadEvent& evt);
    void on_agent_gen_progress(wxThreadEvent& evt);
    void on_agent_history_msg_added(wxThreadEvent& evt);
    void on_agent_history_msg_deleted(wxThreadEvent& evt);
    void on_agent_preset_changed(wxThreadEvent& evt);

private:
    LocusFrame& frame_;
};

} // namespace locus
