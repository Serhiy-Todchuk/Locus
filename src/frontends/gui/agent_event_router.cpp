#include "agent_event_router.h"

#include "ask_user_dialog.h"
#include "chat_panel.h"
#include "locus_frame.h"
#include "locus_tray.h"
#include "memory_bank_panel.h"
#include "notification_sounds.h"
#include "ops_status_view.h"
#include "tool_approval_dialog.h"
#include "activity_panel.h"
#include "file_tree_panel.h"
#include "wx_frontend.h"
#include "endpoint_tooltip.h"
#include "../../llm/endpoint_profile.h"

#include "../../agent/activity_log.h"
#include "../../agent/agent_mode.h"
#include "../../core/frontend.h"
#include "../../core/locus_tab.h"
#include "../../core/workspace.h"
#include "../../tools/permission_presets.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

namespace locus {
void AgentEventRouter::on_agent_turn_start(wxThreadEvent& evt)
{
    int tab_id = evt.GetId();
    frame_.SetStatusText("Agent working...", 0);
    if (frame_.tray_) frame_.tray_->set_state(LocusTray::State::active);
    if (auto* ui = frame_.find_tab_ui(tab_id)) {
        ui->busy = true;
        frame_.refresh_tab_title(tab_id);
        ui->chat->on_turn_start();
    }
}

void AgentEventRouter::on_agent_token(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId()))
        ui->chat->on_token(evt.GetString());
}

void AgentEventRouter::on_agent_reasoning_token(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId()))
        ui->chat->on_reasoning_token(evt.GetString());
}

void AgentEventRouter::on_agent_tool_pending(wxThreadEvent& evt)
{
    int tab_id = evt.GetId();
    auto* ui = frame_.find_tab_ui(tab_id);
    if (!ui) return;
    try {
        auto payload = nlohmann::json::parse(evt.GetString().ToUTF8().data());
        std::string tool    = payload.value("tool", "");
        std::string call_id = payload.value("id", "");
        std::string preview = payload.value("preview", "");
        nlohmann::json args = payload.value("args", nlohmann::json::object());
        std::vector<std::string> safety_warnings;
        if (payload.contains("safety_warnings") && payload["safety_warnings"].is_array()) {
            for (const auto& w : payload["safety_warnings"]) {
                if (w.is_string()) safety_warnings.push_back(w.get<std::string>());
            }
        }
        bool needs_approval = payload.value("needs_approval", true);

        ui->chat->on_tool_pending(
            wxString::FromUTF8(call_id),
            wxString::FromUTF8(tool),
            wxString::FromUTF8(preview),
            args);

        if (!needs_approval) return;

        ui->awaiting_decision = true;
        frame_.refresh_tab_title(tab_id);

        auto* agent_ptr = &ui->tab->agent();
        const auto& cfg = frame_.workspace_.config();
        if (tool == "ask_user") {
            notification_sounds::play(
                notification_sounds::Kind::ask_user, cfg, &frame_);
            std::string question = args.value("question", "");
            AskUserDialog dlg(&frame_, question);
            if (dlg.ShowModal() == wxID_OK) {
                nlohmann::json modified = args;
                modified["response"] = dlg.response();
                agent_ptr->tool_decision(call_id, ToolDecision::modify, modified);
            } else {
                agent_ptr->tool_decision(call_id, ToolDecision::reject, {});
            }
        } else {
            notification_sounds::play(
                notification_sounds::Kind::tool_approval, cfg, &frame_);
            ToolApprovalDialog::run(&frame_, call_id, tool, args, preview,
                safety_warnings,
                [agent_ptr](const std::string& cid, ToolDecision d,
                            const nlohmann::json& modified) {
                    agent_ptr->tool_decision(cid, d, modified);
                });
        }
        ui->awaiting_decision = false;
        frame_.refresh_tab_title(tab_id);
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse tool_pending payload: {}", ex.what());
    }
}

void AgentEventRouter::on_agent_tool_result(wxThreadEvent& evt)
{
    auto* ui = frame_.find_tab_ui(evt.GetId());
    if (!ui) return;
    try {
        auto payload = nlohmann::json::parse(evt.GetString().ToUTF8().data());
        wxString call_id = wxString::FromUTF8(payload.value("call_id", ""));
        wxString display = wxString::FromUTF8(payload.value("display", ""));
        bool success = payload.value("success", true);
        ui->chat->on_tool_result(call_id, display, success);
    } catch (...) {}
}

void AgentEventRouter::on_agent_turn_complete(wxThreadEvent& evt)
{
    int tab_id = evt.GetId();
    frame_.SetStatusText("Ready", 0);
    if (frame_.tray_) frame_.tray_->set_state(LocusTray::State::idle);
    if (auto* ui = frame_.find_tab_ui(tab_id)) {
        ui->busy = false;
        frame_.refresh_tab_title(tab_id);
        ui->chat->on_turn_complete();
        // S6.13 follow-up -- ensure Commit-now disappears at end of turn
        // even if no explicit watchdog_cleared event was emitted (e.g. when
        // a tool call landed naturally and the watchdog never tripped).
        ui->chat->set_commit_now_visible(false);
    }
    notification_sounds::play(
        notification_sounds::Kind::turn_complete,
        frame_.workspace_.config(), &frame_);
    // S5.I -- a tab's title might have just been autoderived from its first
    // user message; refresh the notebook label.
    if (auto* ui = frame_.find_tab_ui(tab_id); ui && ui->tab)
        frame_.refresh_tab_title(tab_id);
}

void AgentEventRouter::on_agent_context_meter(wxThreadEvent& evt)
{
    auto* ui = frame_.find_tab_ui(evt.GetId());
    if (!ui) return;
    int used  = evt.GetInt();
    int limit = static_cast<int>(evt.GetExtraLong());
    int prompt = 0, completion = 0, reserve = 0;
    long long stream_ms = 0;
    auto payload_str = evt.GetString().ToUTF8();
    if (payload_str.length() > 0) {
        try {
            auto j = nlohmann::json::parse(std::string(payload_str.data(), payload_str.length()));
            prompt     = j.value("prompt", 0);
            completion = j.value("completion", 0);
            reserve    = j.value("reserve", 0);
            stream_ms  = j.value("stream_ms", 0LL);
        } catch (...) {}
    }
    ui->chat->set_context_meter(used, limit, prompt, completion, reserve, stream_ms);

    int effective = limit - reserve;
    if (effective <= 0) effective = limit > 0 ? limit : 1;
    double ratio = static_cast<double>(used) / static_cast<double>(effective);
    bool was_warn = ui->ctx_over_warn;
    bool was_auto = ui->ctx_over_auto;
    ui->ctx_over_warn = ratio >= frame_.workspace_.config().compaction.warn_threshold;
    ui->ctx_over_auto = ratio >= frame_.workspace_.config().compaction.auto_threshold;
    if (was_warn != ui->ctx_over_warn || was_auto != ui->ctx_over_auto)
        frame_.refresh_tab_title(evt.GetId());
}

void AgentEventRouter::on_agent_compaction(wxThreadEvent& /*evt*/)
{
    notification_sounds::play(
        notification_sounds::Kind::compaction,
        frame_.workspace_.config(), &frame_);
    frame_.show_compaction_dialog();
}

void AgentEventRouter::on_agent_compaction_archived(wxThreadEvent& evt)
{
    int counter = evt.GetInt();
    // S6.18 C.3 -- no-op count piggybacked via ExtraLong.
    int no_op_count = static_cast<int>(evt.GetExtraLong());
    auto* ui = frame_.find_tab_ui(evt.GetId());
    if (!ui || !ui->chat || !ui->tab) return;
    auto archive_dir = frame_.workspace_.root() / ".locus" / "sessions" /
                       ui->tab->session_id();
    ui->chat->set_compacted_count(counter, no_op_count,
        wxString::FromUTF8(archive_dir.string()));
}

void AgentEventRouter::on_agent_round_progress(wxThreadEvent& evt)
{
    int round      = evt.GetInt();
    int max_rounds = static_cast<int>(evt.GetExtraLong());
    if (auto* ui = frame_.find_tab_ui(evt.GetId())) {
        if (ui->chat) ui->chat->set_round_progress(round, max_rounds);
    }
}

void AgentEventRouter::on_agent_llm_retry(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId())) {
        if (ui->chat) ui->chat->set_transient_status(evt.GetString());
    }
}

void AgentEventRouter::on_agent_unverified_success(wxThreadEvent& evt)
{
    // S6.21 Task 3 -- non-blocking tripwire. Reuse the footer transient-status
    // line (same surface as the LLM-retry notice); the warning is also in the
    // Activity panel so it persists past the next token event that supersedes
    // the footer.
    if (auto* ui = frame_.find_tab_ui(evt.GetId())) {
        if (ui->chat)
            ui->chat->set_transient_status("Unverified: " + evt.GetString());
    }
}

void AgentEventRouter::on_agent_watchdog_tripped(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId())) {
        if (ui->chat) ui->chat->set_commit_now_visible(true);
    }
    // Status bar hint so the user knows why the button just appeared. The
    // trigger string is short ("chars" or "seconds"); value is the count
    // that crossed the configured threshold.
    frame_.SetStatusText(wxString::Format("Watchdog tripped (%s = %d)",
                                    evt.GetString(), evt.GetInt()), 0);
}

void AgentEventRouter::on_agent_watchdog_cleared(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId())) {
        if (ui->chat) ui->chat->set_commit_now_visible(false);
    }
}

void AgentEventRouter::on_agent_endpoint_changed(wxThreadEvent& evt)
{
    auto payload = evt.GetPayload<EndpointChangedPayload>();
    if (auto* ui = frame_.find_tab_ui(evt.GetId()); ui && ui->chat) {
        ui->chat->set_active_endpoint(payload.name);
        // Refresh tooltip from the store so the masked key + url match the
        // now-active profile.
        EndpointProfileStore store;
        store.load();
        if (const auto* prof = store.find(payload.name))
            ui->chat->set_endpoint_tooltip(gui::endpoint_chip_tooltip(*prof));
    }
    frame_.SetStatusText(
        wxString::Format("Endpoint: %s (%s)",
                         wxString::FromUTF8(payload.name),
                         payload.model.empty()
                             ? wxString("server default")
                             : wxString::FromUTF8(payload.model)),
        0);
}

void AgentEventRouter::on_agent_session_reset(wxThreadEvent& evt)
{
    frame_.SetStatusText("Conversation reset", 0);
    if (auto* ui = frame_.find_tab_ui(evt.GetId())) {
        ui->chat->on_session_reset();
        ui->chat->set_system_prompt_bubble(ui->tab->agent().context().system_prompt());
    }
    if (frame_.activity_panel_) frame_.activity_panel_->clear();
}

void AgentEventRouter::on_agent_error(wxThreadEvent& evt)
{
    wxString msg = evt.GetString();
    frame_.SetStatusText("Error: " + msg, 0);
    if (frame_.tray_) frame_.tray_->set_state(LocusTray::State::error);
    if (auto* ui = frame_.find_tab_ui(evt.GetId())) ui->chat->on_error(msg);
    spdlog::error("Agent error (shown in UI): {}", msg.ToStdString());
}
void AgentEventRouter::on_agent_embedding_progress(wxThreadEvent& evt)
{
    int done = evt.GetInt();
    int total = static_cast<int>(evt.GetExtraLong());
    frame_.file_tree_panel_->set_embedding_progress(done, total);
    frame_.ops_status_.set_embedding(done, total);
    frame_.refresh_ops_status();
}

void AgentEventRouter::on_agent_indexing_progress(wxThreadEvent& evt)
{
    int done = evt.GetInt();
    int total = static_cast<int>(evt.GetExtraLong());
    frame_.ops_status_.set_indexing(done, total);
    frame_.refresh_ops_status();
    if (frame_.tray_) {
        const bool agent_busy = std::any_of(
            frame_.tabs_ui_.begin(), frame_.tabs_ui_.end(),
            [](const auto& entry) { return entry.second.busy; });
        if (!agent_busy) {
            frame_.tray_->set_state(total > 0 && done < total
                ? LocusTray::State::indexing
                : LocusTray::State::idle);
        }
    }
    if (total > 0 && done >= total) {
        frame_.refresh_index_stats();
        if (frame_.file_tree_panel_) frame_.file_tree_panel_->rebuild();
    }
}

void AgentEventRouter::on_agent_activity(wxThreadEvent& evt)
{
    // Activity is workspace-shared; route to the single panel regardless of
    // source tab.
    auto ev = evt.GetPayload<ActivityEvent>();
    if (frame_.activity_panel_) frame_.activity_panel_->append(ev);
    if (frame_.memory_bank_panel_) frame_.memory_bank_panel_->on_activity_event(ev);
    // S6.18 C.4 -- per-tab auto-corrections chip. The source tab id rides
    // on evt.GetId(); a quality_correction event increments the chip
    // counter on that tab's chat panel only.
    if (ev.kind == ActivityKind::quality_correction) {
        if (auto* ui = frame_.find_tab_ui(evt.GetId())) {
            if (ui->chat) ui->chat->bump_auto_correction_count();
        }
    }
}

void AgentEventRouter::on_agent_activity_updated(wxThreadEvent& evt)
{
    // ActivityLog coalesced an index_event onto an existing row -- replace
    // the matching row in place. Memory-bank panel doesn't render the
    // activity log, so no notification needed there.
    auto ev = evt.GetPayload<ActivityEvent>();
    if (frame_.activity_panel_) frame_.activity_panel_->update(ev);
}

void AgentEventRouter::on_agent_attached_context(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId()))
        ui->chat->set_attached_chip(evt.GetString());
}

void AgentEventRouter::on_agent_mode_changed(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId())) {
        auto mode = static_cast<AgentMode>(evt.GetInt());
        ui->chat->on_mode_changed(mode);
    }
}

void AgentEventRouter::on_agent_plan_proposed(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId()))
        ui->chat->on_plan_proposed(evt.GetString());
    frame_.refresh_active_tab_footer_status();
}

void AgentEventRouter::on_agent_plan_step_advanced(wxThreadEvent& evt)
{
    auto* ui = frame_.find_tab_ui(evt.GetId());
    if (!ui) return;
    try {
        auto j = nlohmann::json::parse(evt.GetString().utf8_string());
        wxString plan_id = wxString::FromUTF8(j.value("plan_id", ""));
        int      step    = j.value("step_idx", 0);
        wxString status  = wxString::FromUTF8(j.value("status", "pending"));
        wxString notes   = wxString::FromUTF8(j.value("notes", ""));
        ui->chat->on_plan_step_advanced(plan_id, step, status, notes);
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse plan_step_advanced payload: {}", ex.what());
    }
    frame_.refresh_active_tab_footer_status();
}

void AgentEventRouter::on_agent_plan_completed(wxThreadEvent& evt)
{
    auto* ui = frame_.find_tab_ui(evt.GetId());
    if (!ui) return;
    try {
        auto j = nlohmann::json::parse(evt.GetString().utf8_string());
        wxString plan_id = wxString::FromUTF8(j.value("plan_id", ""));
        bool     success = j.value("success", false);
        ui->chat->on_plan_completed(plan_id, success);
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse plan_completed payload: {}", ex.what());
    }
    frame_.refresh_active_tab_footer_status();
}

void AgentEventRouter::on_agent_auto_commit(wxThreadEvent& evt)
{
    auto* ui = frame_.find_tab_ui(evt.GetId());
    if (!ui) return;
    try {
        auto j = nlohmann::json::parse(evt.GetString().utf8_string());
        wxString sha     = wxString::FromUTF8(j.value("short_sha", ""));
        wxString branch  = wxString::FromUTF8(j.value("branch", ""));
        wxString subject = wxString::FromUTF8(j.value("subject", ""));
        ui->chat->on_auto_commit(sha, branch, subject);
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse auto_commit payload: {}", ex.what());
    }
    frame_.refresh_active_tab_footer_status();
}

void AgentEventRouter::on_agent_gen_progress(wxThreadEvent& evt)
{
    auto* ui = frame_.find_tab_ui(evt.GetId());
    if (!ui) return;
    int chars      = evt.GetInt();
    int est_tokens = static_cast<int>(evt.GetExtraLong());
    ui->chat->set_generation_progress(chars, est_tokens);
}

void AgentEventRouter::on_agent_history_msg_added(wxThreadEvent& evt)
{
    auto* ui = frame_.find_tab_ui(evt.GetId());
    if (!ui) return;
    int history_id = evt.GetInt();
    long packed    = evt.GetExtraLong();
    MessageRole role = static_cast<MessageRole>(packed & 0xFF);
    bool deletable   = (packed & 0x100) != 0;
    ui->chat->on_history_message_added(history_id, role, deletable);
    // First user message may have just landed -- autoderived title.
    if (role == MessageRole::user)
        frame_.refresh_tab_title(evt.GetId());
}

void AgentEventRouter::on_agent_history_msg_deleted(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId()))
        ui->chat->on_history_message_deleted(evt.GetInt());
}

void AgentEventRouter::on_agent_preset_changed(wxThreadEvent& evt)
{
    if (auto* ui = frame_.find_tab_ui(evt.GetId())) {
        int packed = evt.GetInt();
        tools::PermissionPreset effective =
            static_cast<tools::PermissionPreset>(packed & 0xff);
        bool from_runtime = (packed & 0x100) != 0;
        ui->chat->on_permission_preset_changed(effective, from_runtime);
    }
}

} // namespace locus
