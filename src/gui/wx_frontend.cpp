#include "wx_frontend.h"

#include <spdlog/spdlog.h>

namespace locus {

// Define the custom event types.
wxDEFINE_EVENT(EVT_AGENT_TOKEN,         wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_TOOL_PENDING,  wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_TOOL_RESULT,   wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_TURN_START,    wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_TURN_COMPLETE, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_CONTEXT_METER, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_COMPACTION,    wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_SESSION_RESET, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_ERROR,         wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_EMBEDDING_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_ACTIVITY,      wxThreadEvent);

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

void WxFrontend::on_tool_call_pending(const ToolCall& call,
                                      const std::string& preview)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_TOOL_PENDING);
    // Pack tool name, call id, args JSON, and preview into the event payload.
    // Use a simple wxStringClientData with JSON-encoded string.
    nlohmann::json payload;
    payload["id"]      = call.id;
    payload["tool"]    = call.tool_name;
    payload["args"]    = call.args;
    payload["preview"] = preview;
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

void WxFrontend::on_activity(const ActivityEvent& event)
{
    auto* evt = new wxThreadEvent(EVT_AGENT_ACTIVITY);
    evt->SetPayload(event);
    wxQueueEvent(handler_, evt);
}

} // namespace locus
