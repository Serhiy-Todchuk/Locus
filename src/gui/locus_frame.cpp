#include "locus_frame.h"
#include "locus_app.h"

#include "../core/watcher_pump.h"
#include "../embedding_worker.h"
#include "../index/indexer.h"

#include <spdlog/spdlog.h>

#include <wx/aboutdlg.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>

namespace locus {

wxBEGIN_EVENT_TABLE(LocusFrame, wxFrame)
    EVT_CLOSE(LocusFrame::on_close)
    EVT_ICONIZE(LocusFrame::on_iconize)
    EVT_AUI_PANE_CLOSE(LocusFrame::on_aui_pane_close)
wxEND_EVENT_TABLE()

LocusFrame::LocusFrame(AgentCore& agent, Workspace& workspace)
    : wxFrame(nullptr, wxID_ANY, "Locus",
              wxDefaultPosition, wxSize(1200, 800))
    , agent_(agent)
    , workspace_(workspace)
{
    // AUI manager
    aui_.SetManagedWindow(this);

    // Menu bar — owned by MenuController; LocusFrame supplies action hooks.
    MenuController::Hooks hooks;
    hooks.on_quit                  = [this] { Close(false); };
    hooks.on_open_workspace_dialog = [this] {
        wxDirDialog dlg(this, "Select workspace folder",
            wxStandardPaths::Get().GetDocumentsDir(),
            wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            std::string path = dlg.GetPath().ToStdString();
            // Defer the switch so this handler returns before the frame is destroyed.
            CallAfter([path] { wxGetApp().open_workspace(path); });
        }
    };
    hooks.on_open_recent = [this](std::string path) {
        CallAfter([path] { wxGetApp().open_workspace(path); });
    };
    hooks.on_settings           = [this] { show_settings_dialog(); };
    hooks.on_reset_conversation = [this] { agent_.reset_conversation(); };
    hooks.on_compact            = [this] { show_compaction_dialog(); };
    hooks.on_save_session       = [this] {
        auto id = agent_.save_session();
        SetStatusText(wxString::Format("Session saved: %s", id));
        if (menu_) menu_->rebuild_sessions_menu(agent_.sessions().list());
    };
    hooks.on_clear_sessions = [this] {
        auto sessions = agent_.sessions().list();
        if (sessions.empty()) {
            SetStatusText("No saved sessions to clear", 0);
            return;
        }
        int answer = wxMessageBox(
            wxString::Format("Delete all %zu saved sessions?\nThis cannot be undone.",
                             sessions.size()),
            "Clear All Sessions", wxYES_NO | wxICON_WARNING, this);
        if (answer != wxYES) return;
        size_t removed = 0;
        for (const auto& s : sessions)
            if (agent_.sessions().remove(s.id)) ++removed;
        SetStatusText(wxString::Format("Deleted %zu session(s)", removed), 0);
        if (menu_) menu_->rebuild_sessions_menu(agent_.sessions().list());
    };
    hooks.on_load_session = [this](std::string id) {
        try {
            agent_.load_session(id);
            SetStatusText(wxString::Format("Loaded session: %s", id), 0);
        } catch (const std::exception& ex) {
            wxMessageBox(wxString::Format("Failed to load session:\n%s", ex.what()),
                         "Load Session", wxOK | wxICON_ERROR, this);
        }
    };
    hooks.on_delete_session = [this](std::string id) {
        int answer = wxMessageBox(
            wxString::Format("Delete session '%s'?\nThis cannot be undone.", id),
            "Delete Session", wxYES_NO | wxICON_WARNING, this);
        if (answer != wxYES) return;
        if (agent_.sessions().remove(id)) {
            SetStatusText(wxString::Format("Deleted session: %s", id), 0);
            if (menu_) menu_->rebuild_sessions_menu(agent_.sessions().list());
        }
    };
    hooks.on_toggle_files_pane    = [this](bool show) { toggle_pane("sidebar",  show); };
    hooks.on_toggle_activity_pane = [this](bool show) { toggle_pane("activity", show); };
    hooks.on_about = [this] {
        wxAboutDialogInfo info;
        info.SetName("Locus");
        info.SetVersion("0.1.0");
        info.SetDescription("Local LLM Agent Assistant");
        info.SetCopyright("MIT License");
        wxAboutBox(info, this);
    };
    hooks.provide_sessions = [this] { return agent_.sessions().list(); };

    menu_ = std::make_unique<MenuController>(this, std::move(hooks));
    menu_->install();

    create_status_bar();
    setup_aui_layout();

    aui_.Update();

    // System tray icon.
    tray_ = std::make_unique<LocusTray>(this);

    // Thread bridge: WxFrontend posts events to this frame.
    wx_frontend_ = std::make_unique<WxFrontend>(this);
    agent_.register_frontend(wx_frontend_.get());

    // Bind agent thread events.
    Bind(EVT_AGENT_TURN_START,    &LocusFrame::on_agent_turn_start,    this);
    Bind(EVT_AGENT_TOKEN,         &LocusFrame::on_agent_token,         this);
    Bind(EVT_AGENT_REASONING_TOKEN, &LocusFrame::on_agent_reasoning_token, this);
    Bind(EVT_AGENT_TOOL_PENDING,  &LocusFrame::on_agent_tool_pending,  this);
    Bind(EVT_AGENT_TOOL_RESULT,   &LocusFrame::on_agent_tool_result,   this);
    Bind(EVT_AGENT_TURN_COMPLETE, &LocusFrame::on_agent_turn_complete, this);
    Bind(EVT_AGENT_CONTEXT_METER, &LocusFrame::on_agent_context_meter, this);
    Bind(EVT_AGENT_COMPACTION,    &LocusFrame::on_agent_compaction,    this);
    Bind(EVT_AGENT_SESSION_RESET, &LocusFrame::on_agent_session_reset, this);
    Bind(EVT_AGENT_ERROR,         &LocusFrame::on_agent_error,         this);
    Bind(EVT_AGENT_EMBEDDING_PROGRESS, &LocusFrame::on_agent_embedding_progress, this);
    Bind(EVT_AGENT_INDEXING_PROGRESS,  &LocusFrame::on_agent_indexing_progress,  this);
    Bind(EVT_AGENT_ACTIVITY,      &LocusFrame::on_agent_activity,      this);
    Bind(EVT_AGENT_ATTACHED_CONTEXT, &LocusFrame::on_agent_attached_context, this);

    // Show LOCUS.md token cost if present.
    if (!workspace_.locus_md().empty()) {
        // Rough estimate: ~4 chars per token.
        int locus_tokens = static_cast<int>(workspace_.locus_md().size()) / 4;
        chat_panel_->set_locus_md_tokens(locus_tokens);
    }

    // Wire embedding worker progress to the WxFrontend thread bridge.
    if (workspace_.embedding_worker()) {
        workspace_.embedding_worker()->on_progress = [this](int done, int total) {
            wx_frontend_->on_embedding_progress(done, total);
        };
    }

    // Wire indexer progress to the WxFrontend thread bridge. Initial build
    // runs before the frame exists; this catches subsequent `process_events`
    // batches from the WatcherPump (which lives in the workspace).
    workspace_.indexer().on_progress = [this](int done, int total) {
        wx_frontend_->on_indexing_progress(done, total);
    };

    // Populate index stats in the file tree panel.
    refresh_index_stats();

    Centre();
    spdlog::info("LocusFrame created");
}

LocusFrame::~LocusFrame()
{
    // Idempotent: no-op if open_workspace already detached us before
    // queueing Destroy(). On the normal-shutdown path the frame is
    // destroyed before LocusApp::OnExit resets the session, so the call
    // here finds workspace_ + agent_ still alive and does the real work.
    detach_from_session();
    aui_.UnInit();
}

void LocusFrame::detach_from_session()
{
    if (session_detached_) return;
    session_detached_ = true;

    // Order: stop the indexer / embedding worker from broadcasting first
    // (their threads can fire callbacks at any moment), then unregister the
    // wx_frontend_ from the agent so no further wxThreadEvents land in the
    // wx event queue. wx_frontend_ itself is freed by the unique_ptr after
    // ~LocusFrame body returns, which is fine -- nothing references it once
    // we're past this point.
    if (workspace_.embedding_worker())
        workspace_.embedding_worker()->on_progress = nullptr;
    workspace_.indexer().on_progress = nullptr;

    agent_.unregister_frontend(wx_frontend_.get());
}

// ---------------------------------------------------------------------------
// View menu / pane visibility
// ---------------------------------------------------------------------------

void LocusFrame::toggle_pane(const wxString& pane_name, bool show)
{
    auto& pane = aui_.GetPane(pane_name);
    if (!pane.IsOk()) return;
    pane.Show(show);
    aui_.Update();
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void LocusFrame::create_status_bar()
{
    CreateStatusBar(2);
    // Right pane (background ops) wider than left pane (general status).
    // Negative values are proportional — left takes 1 share, right takes 3.
    const int widths[] = { -1, -3 };
    SetStatusWidths(2, widths);
    SetStatusText("Ready", 0);
    SetStatusText("", 1);
}

// ---------------------------------------------------------------------------
// AUI layout: 3-pane shell
// ---------------------------------------------------------------------------

void LocusFrame::setup_aui_layout()
{
    // Left sidebar: file tree with index stats.
    file_tree_panel_ = new FileTreePanel(this, workspace_.query(),
        workspace_.root().string(),
        [this](const std::string& path) {
            spdlog::trace("File selected in tree: {}", path);
            SetStatusText(wxString::FromUTF8(path), 0);
        },
        // Right-click → "Attach to context" hands the workspace-relative path
        // to AgentCore; the chip update fans back via on_attached_context_changed.
        [this](const std::string& path) {
            agent_.set_attached_context({path, /*preview*/{}});
        });

    // Center chat panel.
    chat_panel_ = new ChatPanel(this,
        [this](const std::string& msg) {
            // Flush any pending file-watcher events first so the agent's
            // first tool call sees a fresh index.
            workspace_.watcher_pump().flush_now();
            refresh_index_stats();
            agent_.send_message(msg);
        },
        [this]() { show_compaction_dialog(); },
        [this]() { agent_.cancel_turn(); },
        [this]() {
            // Undo the most recent checkpointed turn. Surface the summary as
            // a system note in chat — the same string the /undo command emits.
            std::string summary = agent_.undo_turn();
            wxString html = "<pre>" + wxString::FromUTF8(summary) + "</pre>";
            chat_panel_->append_system_note(html);
        });

    // Chip "✕" → detach the pinned file from the conversation context.
    chat_panel_->set_on_detach([this]() {
        agent_.clear_attached_context();
    });

    // Slash-command suggestions: CLI-style commands + all registered tools.
    {
        std::vector<SlashItem> items = {
            {"help",     "Show available slash commands",         false},
            {"reset",    "Start a fresh conversation",            false},
            {"compact",  "Open the context compaction dialog",    false},
            {"save",     "Save the current session to disk",      false},
            {"settings", "Open the Settings dialog",              false},
            {"clear",    "Alias for /reset",                      false},
            {"undo",     "Revert files mutated by the last turn", false},
        };
        for (const auto& c : agent_.slash_dispatcher().complete("")) {
            std::string desc = c.signature.empty()
                ? c.description
                : c.signature + "  -  " + c.description;
            items.push_back({ c.name, std::move(desc), true });
        }
        chat_panel_->set_slash_commands(std::move(items));
    }

    chat_panel_->set_on_slash_command(
        [this](const std::string& name, const std::string& rest) -> bool {
            if (name == "reset" || name == "clear") {
                agent_.reset_conversation();
                return true;
            }
            if (name == "compact") {
                show_compaction_dialog();
                return true;
            }
            if (name == "save") {
                auto id = agent_.save_session();
                SetStatusText(wxString::Format("Session saved: %s", id));
                if (menu_) menu_->rebuild_sessions_menu(agent_.sessions().list());
                return true;
            }
            if (name == "settings") {
                show_settings_dialog();
                return true;
            }
            if (name == "help") {
                wxString html =
                    "<span class=\"tool-name\">Slash commands</span><br>"
                    "<span class=\"tool-preview\">"
                    "/help&nbsp;&nbsp;— show this help<br>"
                    "/reset | /clear&nbsp;&nbsp;— reset conversation<br>"
                    "/compact&nbsp;&nbsp;— compact context<br>"
                    "/save&nbsp;&nbsp;— save session<br>"
                    "/settings&nbsp;&nbsp;— open settings<br>"
                    "/&lt;tool&gt; &lt;args&gt;&nbsp;&nbsp;— type a tool name"
                    " to describe a call to the agent"
                    "</span>";
                chat_panel_->append_system_note(html);
                return true;
            }
            // Tool names (and unknown commands): fall through to a normal
            // user message so the agent/LLM can decide what to do.
            (void)rest;
            return false;
        });

    // Tool approval panel — slides in when a tool call needs approval.
    approval_panel_ = new ToolApprovalPanel(this,
        [this](const std::string& call_id, ToolDecision decision,
               const nlohmann::json& modified_args) {
            agent_.tool_decision(call_id, decision, modified_args);
        });

    // Right detail panel — Activity log (S2.2).
    activity_panel_ = new ActivityPanel(this, agent_);

    // Add panes.
    aui_.AddPane(file_tree_panel_, wxAuiPaneInfo()
        .Name("sidebar").Caption("Files")
        .Left().MinSize(180, -1).BestSize(240, -1)
        .CloseButton(true).PinButton(true));

    aui_.AddPane(chat_panel_, wxAuiPaneInfo()
        .Name("chat").Caption("Chat")
        .CenterPane());

    aui_.AddPane(approval_panel_, wxAuiPaneInfo()
        .Name("approval").Caption("Tool Approval")
        .Bottom().MinSize(-1, 180).BestSize(-1, 220)
        .CloseButton(false).PinButton(false)
        .Hide());  // shown dynamically when a tool call needs approval

    aui_.AddPane(activity_panel_, wxAuiPaneInfo()
        .Name("activity").Caption("Activity")
        .Right().MinSize(280, -1).BestSize(420, -1)
        .CloseButton(true).PinButton(true));
}

// ---------------------------------------------------------------------------
// Compaction dialog
// ---------------------------------------------------------------------------

void LocusFrame::show_compaction_dialog()
{
    int used  = static_cast<int>(agent_.history().estimate_tokens());
    int limit = agent_.context_limit();

    CompactionDialog dlg(this, used, limit, agent_.history());
    if (dlg.ShowModal() == wxID_OK) {
        auto choice = dlg.result();
        if (choice.made) {
            agent_.compact_context(choice.strategy, choice.drop_n);
            SetStatusText("Context compacted", 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Settings dialog
// ---------------------------------------------------------------------------

void LocusFrame::show_settings_dialog()
{
    SettingsDialog dlg(this, workspace_.config(), agent_.tools());
    if (dlg.ShowModal() == wxID_OK && dlg.config_changed()) {
        workspace_.save_config();
        SetStatusText("Settings saved", 0);

        if (dlg.llm_changed()) {
            wxMessageBox(
                "LLM settings changed.\nRestart Locus to apply new endpoint/model settings.",
                "Settings", wxOK | wxICON_INFORMATION, this);
        }

        if (dlg.semantic_changed()) {
            // Always disable first (handles model name change while enabled)
            workspace_.disable_semantic_search();

            if (workspace_.config().semantic_search_enabled) {
                if (!workspace_.enable_semantic_search()) {
                    wxMessageBox(
                        "Could not enable semantic search.\n"
                        "Check that the GGUF model file exists in the models/ folder\n"
                        "next to the Locus executable.",
                        "Semantic Search", wxOK | wxICON_WARNING, this);
                }
            }
        }

        spdlog::info("Settings saved to config.json");
    }
}

// ---------------------------------------------------------------------------
// Index stats
// ---------------------------------------------------------------------------

void LocusFrame::refresh_index_stats()
{
    auto& st = workspace_.indexer().stats();
    file_tree_panel_->set_index_stats(
        st.files_total, st.symbols_total, st.headings_total);
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

void LocusFrame::on_close(wxCloseEvent& evt)
{
    // The embedding worker runs on its own thread and fires on_progress
    // callbacks into this frame. Stop it here -- before destruction -- so no
    // callback can fire into freed memory. stop() joins the thread.
    if (auto* ew = workspace_.embedding_worker()) {
        ew->on_progress = nullptr;
        ew->stop();
    }

    // Remove tray icon before destroying frame.
    if (tray_)
        tray_->RemoveIcon();
    tray_.reset();

    // Detach session-dependent refs synchronously, while workspace_ + agent_
    // are guaranteed alive. wx queues the actual ~LocusFrame for an idle
    // event; on the normal app-shutdown path that idle event fires AFTER
    // LocusApp::OnExit has already reset the session, leaving workspace_ /
    // agent_ dangling. detach_from_session() is idempotent, so the
    // destructor's call later just early-returns.
    detach_from_session();

    evt.Skip();  // proceed with destruction
}

// ---------------------------------------------------------------------------
// Window state
// ---------------------------------------------------------------------------

void LocusFrame::on_iconize(wxIconizeEvent& evt)
{
    if (evt.IsIconized()) {
        // Minimize to tray: hide the window, tray icon remains.
        Hide();
    }
    evt.Skip();
}

void LocusFrame::on_aui_pane_close(wxAuiManagerEvent& evt)
{
    // User clicked the pane's X — mirror the change in the View menu.
    if (auto* pane = evt.GetPane(); pane && menu_) {
        if (pane->name == "sidebar")
            menu_->set_files_pane_visible(false);
        else if (pane->name == "activity")
            menu_->set_activity_pane_visible(false);
    }
    evt.Skip();
}

// ---------------------------------------------------------------------------
// Agent thread events — UI thread handlers
// ---------------------------------------------------------------------------

void LocusFrame::on_agent_turn_start(wxThreadEvent& /*evt*/)
{
    SetStatusText("Agent working...", 0);
    if (tray_) tray_->set_state(LocusTray::State::active);
    chat_panel_->on_turn_start();
}

void LocusFrame::on_agent_token(wxThreadEvent& evt)
{
    chat_panel_->on_token(evt.GetString());
}

void LocusFrame::on_agent_reasoning_token(wxThreadEvent& evt)
{
    chat_panel_->on_reasoning_token(evt.GetString());
}

void LocusFrame::on_agent_tool_pending(wxThreadEvent& evt)
{
    try {
        auto payload = nlohmann::json::parse(evt.GetString().ToStdString());
        std::string tool    = payload.value("tool", "");
        std::string call_id = payload.value("id", "");
        std::string preview = payload.value("preview", "");
        nlohmann::json args = payload.value("args", nlohmann::json::object());

        // Show tool call inline in chat.
        chat_panel_->on_tool_pending(
            wxString::FromUTF8(tool), wxString::FromUTF8(preview));

        if (tool == "ask_user") {
            // Pop-up modal — disappears immediately after answer/cancel.
            std::string question = args.value("question", "");
            AskUserDialog dlg(this, question);
            if (dlg.ShowModal() == wxID_OK) {
                nlohmann::json modified = args;
                modified["response"] = dlg.response();
                agent_.tool_decision(call_id, ToolDecision::modify, modified);
            } else {
                agent_.tool_decision(call_id, ToolDecision::reject, {});
            }
        } else {
            // Show approval panel and update AUI.
            approval_panel_->show_tool_call(call_id, tool, args, preview);
            aui_.GetPane("approval").Show();
            aui_.Update();
        }
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse tool_pending payload: {}", ex.what());
    }
}

void LocusFrame::on_agent_tool_result(wxThreadEvent& evt)
{
    // Hide approval panel (tool was executed).
    if (approval_panel_->IsShown()) {
        approval_panel_->dismiss();
        aui_.GetPane("approval").Hide();
        aui_.Update();
    }

    try {
        auto payload = nlohmann::json::parse(evt.GetString().ToStdString());
        wxString display = wxString::FromUTF8(payload.value("display", ""));
        chat_panel_->on_tool_result(display);
    } catch (...) {}
}

void LocusFrame::on_agent_turn_complete(wxThreadEvent& /*evt*/)
{
    SetStatusText("Ready", 0);
    if (tray_) tray_->set_state(LocusTray::State::idle);

    // Ensure approval panel is hidden.
    if (approval_panel_->IsShown()) {
        approval_panel_->dismiss();
        aui_.GetPane("approval").Hide();
        aui_.Update();
    }

    chat_panel_->on_turn_complete();
}

void LocusFrame::on_agent_context_meter(wxThreadEvent& evt)
{
    int used  = evt.GetInt();
    int limit = static_cast<int>(evt.GetExtraLong());
    chat_panel_->set_context_meter(used, limit);
}

void LocusFrame::on_agent_compaction(wxThreadEvent& /*evt*/)
{
    show_compaction_dialog();
}

void LocusFrame::on_agent_session_reset(wxThreadEvent& /*evt*/)
{
    SetStatusText("Conversation reset", 0);
    chat_panel_->on_session_reset();
    if (activity_panel_) activity_panel_->clear();
}

void LocusFrame::on_agent_error(wxThreadEvent& evt)
{
    wxString msg = evt.GetString();
    SetStatusText("Error: " + msg, 0);
    if (tray_) tray_->set_state(LocusTray::State::error);
    chat_panel_->on_error(msg);
    spdlog::error("Agent error (shown in UI): {}", msg.ToStdString());
}

void LocusFrame::on_agent_embedding_progress(wxThreadEvent& evt)
{
    int done = evt.GetInt();
    int total = static_cast<int>(evt.GetExtraLong());
    file_tree_panel_->set_embedding_progress(done, total);

    ops_status_.set_embedding(done, total);
    refresh_ops_status();
}

void LocusFrame::on_agent_indexing_progress(wxThreadEvent& evt)
{
    int done = evt.GetInt();
    int total = static_cast<int>(evt.GetExtraLong());

    ops_status_.set_indexing(done, total);
    refresh_ops_status();

    // Batch finished — pull fresh totals into the file tree footer.
    if (total > 0 && done >= total)
        refresh_index_stats();
}

void LocusFrame::refresh_ops_status()
{
    SetStatusText(ops_status_.compose(), 1);
}

void LocusFrame::on_agent_activity(wxThreadEvent& evt)
{
    auto ev = evt.GetPayload<ActivityEvent>();
    if (activity_panel_) activity_panel_->append(ev);
}

void LocusFrame::on_agent_attached_context(wxThreadEvent& evt)
{
    if (chat_panel_)
        chat_panel_->set_attached_chip(evt.GetString());
}

} // namespace locus
