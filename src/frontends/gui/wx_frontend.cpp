#include "wx_frontend.h"

#include <spdlog/spdlog.h>

namespace locus {

// Define the custom event types.
wxDEFINE_EVENT(EVT_AGENT_TOKEN,         wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_REASONING_TOKEN, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_TOOL_PENDING,  wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_TOOL_RESULT,   wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_TURN_START,    wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_TURN_COMPLETE, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_CONTEXT_METER, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_COMPACTION,    wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_SESSION_RESET, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_ERROR,         wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_EMBEDDING_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_INDEXING_PROGRESS,  wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_ACTIVITY,      wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_ATTACHED_CONTEXT, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_MODE_CHANGED,        wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_PLAN_PROPOSED,       wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_PLAN_STEP_ADVANCED,  wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_PLAN_COMPLETED,      wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_AUTO_COMMIT,         wxThreadEvent);

WxFrontend::WxFrontend(wxEvtHandler* handler)
    : handler_(handler)
{
}

void WxFrontend::on_turn_start()
{
    auto* evt = new wxThreadEvent(EVT_AGENT_TURN_START);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_token(std::string_view token)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_TOKEN);
    evt->SetString(wxString::FromUTF8(token.data(), token.size()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_reasoning_token(std::string_view token)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_REASONING_TOKEN);
    evt->SetString(wxString::FromUTF8(token.data(), token.size()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_tool_call_pending(const ToolCall& call,
                                      const std::string& preview,
                                      bool needs_approval)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_TOOL_PENDING);
    // Pack tool name, call id, args JSON, preview, and approval flag into
    // the event payload. The approval flag lets the frame skip popping the
    // approval panel for auto-approved calls (which still need to render in
    // chat -- the dispatcher fires this for ALL calls, not just gated ones).
    nlohmann::json payload;
    payload["id"]              = call.id;
    payload["tool"]            = call.tool_name;
    payload["args"]            = call.args;
    payload["preview"]         = preview;
    payload["needs_approval"]  = needs_approval;
    evt->SetString(wxString::FromUTF8(payload.dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_tool_result(const std::string& call_id,
                                const std::string& display)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_TOOL_RESULT);
    nlohmann::json payload;
    payload["call_id"] = call_id;
    payload["display"] = display;
    evt->SetString(wxString::FromUTF8(payload.dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_turn_complete()
{
    auto* evt = new wxThreadEvent(EVT_AGENT_TURN_COMPLETE);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_context_meter(int used_tokens, int limit)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_CONTEXT_METER);
    evt->SetInt(used_tokens);
    evt->SetExtraLong(limit);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_compaction_needed(int used_tokens, int limit)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_COMPACTION);
    evt->SetInt(used_tokens);
    evt->SetExtraLong(limit);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_session_reset()
{
    auto* evt = new wxThreadEvent(EVT_AGENT_SESSION_RESET);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_error(const std::string& message)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_ERROR);
    evt->SetString(wxString::FromUTF8(message));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_embedding_progress(int done, int total)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_EMBEDDING_PROGRESS);
    evt->SetInt(done);
    evt->SetExtraLong(total);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_indexing_progress(int done, int total)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_INDEXING_PROGRESS);
    evt->SetInt(done);
    evt->SetExtraLong(total);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_activity(const ActivityEvent& event)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_ACTIVITY);
    evt->SetPayload(event);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_attached_context_changed(
    const std::optional<AttachedContext>& ctx)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_ATTACHED_CONTEXT);
    // Empty string means "detached"; otherwise carry the workspace-relative path.
    evt->SetString(ctx ? wxString::FromUTF8(ctx->file_path) : wxString{});
    wxQueueEvent(handler_, evt);
}

// -- S4.D plan-mode event marshalling ---------------------------------------

void WxFrontend::on_mode_changed(AgentMode mode)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_MODE_CHANGED);
    evt->SetInt(static_cast<int>(mode));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_plan_proposed(const Plan& plan)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_PLAN_PROPOSED);
    // Pack the plan as JSON so the LocusFrame handler doesn't need to know
    // about Plan's internal layout. plan_to_json is the single point of
    // truth for the wire format.
    evt->SetString(wxString::FromUTF8(plan_to_json(plan).dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_plan_step_advanced(const std::string& plan_id, int step_idx,
                                        PlanStep::Status status,
                                        const std::string& notes)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_PLAN_STEP_ADVANCED);
    nlohmann::json j;
    j["plan_id"]  = plan_id;
    j["step_idx"] = step_idx;
    j["status"]   = to_string(status);
    j["notes"]    = notes;
    evt->SetString(wxString::FromUTF8(j.dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_plan_completed(const std::string& plan_id, bool success)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_PLAN_COMPLETED);
    nlohmann::json j;
    j["plan_id"] = plan_id;
    j["success"] = success;
    evt->SetString(wxString::FromUTF8(j.dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_auto_commit(const std::string& short_sha,
                                 const std::string& branch,
                                 const std::string& subject)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_AUTO_COMMIT);
    nlohmann::json j;
    j["short_sha"] = short_sha;
    j["branch"]    = branch;
    j["subject"]   = subject;
    evt->SetString(wxString::FromUTF8(j.dump()));
    wxQueueEvent(handler_, evt);
}

} // namespace locus
