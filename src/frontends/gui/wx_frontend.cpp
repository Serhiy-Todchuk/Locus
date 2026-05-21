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
wxDEFINE_EVENT(EVT_AGENT_COMPACTION_ARCHIVED, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_SESSION_RESET, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_ERROR,         wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_EMBEDDING_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_INDEXING_PROGRESS,  wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_ACTIVITY,      wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_ACTIVITY_UPDATED, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_ATTACHED_CONTEXT, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_MODE_CHANGED,        wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_PLAN_PROPOSED,       wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_PLAN_STEP_ADVANCED,  wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_PLAN_COMPLETED,      wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_AUTO_COMMIT,         wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_GEN_PROGRESS,        wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_HISTORY_MSG_ADDED,   wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_HISTORY_MSG_DELETED, wxThreadEvent);
wxDEFINE_EVENT(EVT_AGENT_PRESET_CHANGED,      wxThreadEvent);

WxFrontend::WxFrontend(wxEvtHandler* handler, int tab_id)
    : handler_(handler), tab_id_(tab_id)
{
}

// Tiny helper: every posted event carries the tab_id via wxEvent::SetId so
// the frame can route to the right ChatPanel without inspecting the source
// WxFrontend instance. tab_id 0 is the legacy "no tab routing" shape.
static inline wxThreadEvent* new_evt(wxEventType type, int tab_id)
{
    auto* evt = new wxThreadEvent(type);
    evt->SetId(tab_id);
    return evt;
}

void WxFrontend::on_turn_start()
{
    wxQueueEvent(handler_, new_evt(EVT_AGENT_TURN_START, tab_id_));
}

void WxFrontend::on_token(std::string_view token)
{
    auto* evt = new_evt(EVT_AGENT_TOKEN, tab_id_);
    evt->SetString(wxString::FromUTF8(token.data(), token.size()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_reasoning_token(std::string_view token)
{
    auto* evt = new_evt(EVT_AGENT_REASONING_TOKEN, tab_id_);
    evt->SetString(wxString::FromUTF8(token.data(), token.size()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_tool_call_pending(const ToolCall& call,
                                      const std::string& preview,
                                      bool needs_approval,
                                      const std::vector<std::string>& safety_warnings)
{
    auto* evt = new_evt(EVT_AGENT_TOOL_PENDING, tab_id_);
    nlohmann::json payload;
    payload["id"]              = call.id;
    payload["tool"]            = call.tool_name;
    payload["args"]            = call.args;
    payload["preview"]         = preview;
    payload["needs_approval"]  = needs_approval;
    payload["safety_warnings"] = safety_warnings;
    evt->SetString(wxString::FromUTF8(payload.dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_tool_result(const std::string& call_id,
                                const std::string& display,
                                bool success)
{
    auto* evt = new_evt(EVT_AGENT_TOOL_RESULT, tab_id_);
    nlohmann::json payload;
    payload["call_id"] = call_id;
    payload["display"] = display;
    payload["success"] = success;
    evt->SetString(wxString::FromUTF8(payload.dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_turn_complete()
{
    wxQueueEvent(handler_, new_evt(EVT_AGENT_TURN_COMPLETE, tab_id_));
}

void WxFrontend::on_context_meter(int used_tokens, int limit,
                                   int prompt_tokens, int completion_tokens,
                                   int reserve_tokens,
                                   long long stream_ms_last_round)
{
    auto* evt = new_evt(EVT_AGENT_CONTEXT_METER, tab_id_);
    evt->SetInt(used_tokens);
    evt->SetExtraLong(limit);
    nlohmann::json payload;
    payload["prompt"]     = prompt_tokens;
    payload["completion"] = completion_tokens;
    payload["reserve"]    = reserve_tokens;
    payload["stream_ms"]  = stream_ms_last_round;
    evt->SetString(wxString::FromUTF8(payload.dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_compaction_needed(int used_tokens, int limit)
{
    auto* evt = new_evt(EVT_AGENT_COMPACTION, tab_id_);
    evt->SetInt(used_tokens);
    evt->SetExtraLong(limit);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_compaction_archived(int counter)
{
    auto* evt = new_evt(EVT_AGENT_COMPACTION_ARCHIVED, tab_id_);
    evt->SetInt(counter);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_session_reset()
{
    wxQueueEvent(handler_, new_evt(EVT_AGENT_SESSION_RESET, tab_id_));
}

void WxFrontend::on_error(const std::string& message)
{
    auto* evt = new_evt(EVT_AGENT_ERROR, tab_id_);
    evt->SetString(wxString::FromUTF8(message));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_embedding_progress(int done, int total)
{
    auto* evt = new_evt(EVT_AGENT_EMBEDDING_PROGRESS, tab_id_);
    evt->SetInt(done);
    evt->SetExtraLong(total);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_indexing_progress(int done, int total)
{
    auto* evt = new_evt(EVT_AGENT_INDEXING_PROGRESS, tab_id_);
    evt->SetInt(done);
    evt->SetExtraLong(total);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_activity(const ActivityEvent& event)
{
    auto* evt = new_evt(EVT_AGENT_ACTIVITY, tab_id_);
    evt->SetPayload(event);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_activity_updated(const ActivityEvent& event)
{
    auto* evt = new_evt(EVT_AGENT_ACTIVITY_UPDATED, tab_id_);
    evt->SetPayload(event);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_attached_context_changed(
    const std::optional<AttachedContext>& ctx)
{
    auto* evt = new_evt(EVT_AGENT_ATTACHED_CONTEXT, tab_id_);
    evt->SetString(ctx ? wxString::FromUTF8(ctx->file_path) : wxString{});
    wxQueueEvent(handler_, evt);
}

// -- S4.D plan-mode event marshalling ---------------------------------------

void WxFrontend::on_mode_changed(AgentMode mode)
{
    auto* evt = new_evt(EVT_AGENT_MODE_CHANGED, tab_id_);
    evt->SetInt(static_cast<int>(mode));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_plan_proposed(const Plan& plan)
{
    auto* evt = new_evt(EVT_AGENT_PLAN_PROPOSED, tab_id_);
    evt->SetString(wxString::FromUTF8(plan_to_json(plan).dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_plan_step_advanced(const std::string& plan_id, int step_idx,
                                        PlanStep::Status status,
                                        const std::string& notes)
{
    auto* evt = new_evt(EVT_AGENT_PLAN_STEP_ADVANCED, tab_id_);
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
    auto* evt = new_evt(EVT_AGENT_PLAN_COMPLETED, tab_id_);
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
    auto* evt = new_evt(EVT_AGENT_AUTO_COMMIT, tab_id_);
    nlohmann::json j;
    j["short_sha"] = short_sha;
    j["branch"]    = branch;
    j["subject"]   = subject;
    evt->SetString(wxString::FromUTF8(j.dump()));
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_generation_progress(int chars, int est_tokens)
{
    auto* evt = new_evt(EVT_AGENT_GEN_PROGRESS, tab_id_);
    evt->SetInt(chars);
    evt->SetExtraLong(est_tokens);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_history_message_added(int history_id, MessageRole role,
                                           bool deletable)
{
    auto* evt = new_evt(EVT_AGENT_HISTORY_MSG_ADDED, tab_id_);
    evt->SetInt(history_id);
    long packed = static_cast<long>(static_cast<int>(role)) & 0xFF;
    if (deletable) packed |= 0x100;
    evt->SetExtraLong(packed);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_history_message_deleted(int history_id)
{
    auto* evt = new_evt(EVT_AGENT_HISTORY_MSG_DELETED, tab_id_);
    evt->SetInt(history_id);
    wxQueueEvent(handler_, evt);
}

void WxFrontend::on_permission_preset_changed(tools::PermissionPreset effective,
                                              bool from_runtime)
{
    auto* evt = new_evt(EVT_AGENT_PRESET_CHANGED, tab_id_);
    // Encode (preset, from_runtime) into the SetInt slot.
    // Low byte = preset enum; bit 8 = from_runtime flag.
    int packed = static_cast<int>(effective) & 0xff;
    if (from_runtime) packed |= 0x100;
    evt->SetInt(packed);
    wxQueueEvent(handler_, evt);
}

} // namespace locus
