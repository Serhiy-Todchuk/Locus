#include "locus_frame.h"
#include "locus_app.h"
#include "recent_workspaces.h"

#include "../embedding_worker.h"
#include "../indexer.h"

#include <spdlog/spdlog.h>

#include <wx/aboutdlg.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>

namespace locus {

enum {
    ID_MENU_QUIT = wxID_EXIT,
    ID_MENU_ABOUT = wxID_ABOUT,
    ID_MENU_RESET = wxID_HIGHEST + 200,
    ID_MENU_COMPACT,
    ID_MENU_SAVE_SESSION,
    ID_MENU_OPEN_WORKSPACE,
    ID_MENU_SETTINGS,
    ID_MENU_VIEW_FILES,
    ID_MENU_VIEW_ACTIVITY,
    ID_MENU_CLEAR_SESSIONS,
    ID_TIMER_WATCHER,
    ID_MENU_RECENT_BASE         = wxID_HIGHEST + 300,  // 300..309 for recent workspaces
    ID_MENU_SESSION_OPEN_BASE   = wxID_HIGHEST + 400,  // 400..449 for session open
    ID_MENU_SESSION_DELETE_BASE = wxID_HIGHEST + 500,  // 500..549 for session delete
};

constexpr size_t k_max_sessions_in_menu = 50;

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

    create_menu_bar();
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
    // batches from the file watcher.
    workspace_.indexer().on_progress = [this](int done, int total) {
        wx_frontend_->on_indexing_progress(done, total);
    };

    // File-watcher pump: drain the watcher every 500 ms and batch events
    // so rapid saves produce one merged index transaction + one Activity row.
    watcher_timer_.SetOwner(this, ID_TIMER_WATCHER);
    Bind(wxEVT_TIMER, &LocusFrame::on_watcher_timer, this, ID_TIMER_WATCHER);
    watcher_timer_.Start(500);

    // Populate index stats in the file tree panel.
    refresh_index_stats();

    Centre();
    spdlog::info("LocusFrame created");
}

LocusFrame::~LocusFrame()
{
    // Detach the embedding-worker progress callback before tearing down
    // wx_frontend_ — the worker runs on its own thread and may still fire
    // progress events after the frame starts destructing.
    if (workspace_.embedding_worker())
        workspace_.embedding_worker()->on_progress = nullptr;
    workspace_.indexer().on_progress = nullptr;

    agent_.unregister_frontend(wx_frontend_.get());
    aui_.UnInit();
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

void LocusFrame::create_menu_bar()
{
    auto* file_menu = new wxMenu;
    file_menu->Append(ID_MENU_OPEN_WORKSPACE, "Open Workspace...\tCtrl+O");

    // Recent Workspaces submenu.
    recent_menu_ = new wxMenu;
    rebuild_recent_menu();
    file_menu->AppendSubMenu(recent_menu_, "Recent Workspaces");

    file_menu->Append(ID_MENU_SETTINGS,       "Settings...\tCtrl+,");
    file_menu->AppendSeparator();
    file_menu->Append(ID_MENU_QUIT, "Quit\tCtrl+Q");

    auto* session_menu = new wxMenu;
    session_menu->Append(ID_MENU_RESET,        "Reset Conversation\tCtrl+R");
    session_menu->Append(ID_MENU_COMPACT,       "Compact Context");
    session_menu->Append(ID_MENU_SAVE_SESSION,  "Save Session\tCtrl+S");
    sessions_menu_ = new wxMenu;
    rebuild_sessions_menu();
    session_menu->AppendSubMenu(sessions_menu_, "Saved Sessions");
    session_menu->AppendSeparator();
    session_menu->Append(ID_MENU_CLEAR_SESSIONS, "Clear All Sessions...");

    // Rebuild the sessions submenu each time the Session menu opens, so newly
    // saved or externally-removed sessions show up without restarting the app.
    session_menu->Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& e) {
        rebuild_sessions_menu();
        e.Skip();
    });

    auto* view_menu = new wxMenu;
    view_files_item_    = view_menu->AppendCheckItem(ID_MENU_VIEW_FILES,    "Files Panel");
    view_activity_item_ = view_menu->AppendCheckItem(ID_MENU_VIEW_ACTIVITY, "Activity Panel");
    view_files_item_->Check(true);
    view_activity_item_->Check(true);

    auto* help_menu = new wxMenu;
    help_menu->Append(ID_MENU_ABOUT, "About...");

    auto* menu_bar = new wxMenuBar;
    menu_bar->Append(file_menu,    "File");
    menu_bar->Append(view_menu,    "View");
    menu_bar->Append(session_menu, "Session");
    menu_bar->Append(help_menu,    "Help");
    SetMenuBar(menu_bar);

    // Menu event handlers.
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(false); }, ID_MENU_QUIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxDirDialog dlg(this, "Select workspace folder",
            wxStandardPaths::Get().GetDocumentsDir(),
            wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            std::string path = dlg.GetPath().ToStdString();
            // Defer the switch so this handler returns before the frame is destroyed.
            CallAfter([path]() {
                wxGetApp().open_workspace(path);
            });
        }
    }, ID_MENU_OPEN_WORKSPACE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        agent_.reset_conversation();
    }, ID_MENU_RESET);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        show_compaction_dialog();
    }, ID_MENU_COMPACT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        auto id = agent_.save_session();
        SetStatusText(wxString::Format("Session saved: %s", id));
        rebuild_sessions_menu();
    }, ID_MENU_SAVE_SESSION);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
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
        rebuild_sessions_menu();
    }, ID_MENU_CLEAR_SESSIONS);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        show_settings_dialog();
    }, ID_MENU_SETTINGS);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        toggle_pane("sidebar", e.IsChecked());
    }, ID_MENU_VIEW_FILES);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        toggle_pane("activity", e.IsChecked());
    }, ID_MENU_VIEW_ACTIVITY);

    // Saved Sessions submenu: bind the whole ID range once.
    Bind(wxEVT_MENU, &LocusFrame::on_session_open,   this,
         ID_MENU_SESSION_OPEN_BASE,
         ID_MENU_SESSION_OPEN_BASE + static_cast<int>(k_max_sessions_in_menu) - 1);
    Bind(wxEVT_MENU, &LocusFrame::on_session_delete, this,
         ID_MENU_SESSION_DELETE_BASE,
         ID_MENU_SESSION_DELETE_BASE + static_cast<int>(k_max_sessions_in_menu) - 1);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxAboutDialogInfo info;
        info.SetName("Locus");
        info.SetVersion("0.1.0");
        info.SetDescription("Local LLM Agent Assistant");
        info.SetCopyright("MIT License");
        wxAboutBox(info, this);
    }, ID_MENU_ABOUT);
}

void LocusFrame::rebuild_recent_menu()
{
    // Clear existing items.
    while (recent_menu_->GetMenuItemCount() > 0)
        recent_menu_->Delete(recent_menu_->FindItemByPosition(0));

    auto entries = RecentWorkspaces::load();
    if (entries.empty()) {
        recent_menu_->Append(wxID_ANY, "(none)")->Enable(false);
        return;
    }

    int id = ID_MENU_RECENT_BASE;
    for (size_t i = 0; i < entries.size() && i < RecentWorkspaces::k_max_entries; ++i, ++id) {
        recent_menu_->Append(id, wxString::FromUTF8(entries[i]));
        Bind(wxEVT_MENU, [this, path = entries[i]](wxCommandEvent&) {
            CallAfter([path]() {
                wxGetApp().open_workspace(path);
            });
        }, id);
    }
}

void LocusFrame::rebuild_sessions_menu()
{
    if (!sessions_menu_) return;

    while (sessions_menu_->GetMenuItemCount() > 0)
        sessions_menu_->Delete(sessions_menu_->FindItemByPosition(0));

    sessions_in_menu_.clear();

    auto sessions = agent_.sessions().list();
    if (sessions.empty()) {
        sessions_menu_->Append(wxID_ANY, "(none)")->Enable(false);
        return;
    }

    const size_t n = std::min(sessions.size(), k_max_sessions_in_menu);
    sessions_in_menu_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& s = sessions[i];

        std::string preview = s.first_user_message;
        if (preview.size() > 48) preview = preview.substr(0, 45) + "...";
        wxString label = wxString::Format(
            "%s  %s  (%d msgs)",
            wxString::FromUTF8(s.timestamp.empty() ? s.id : s.timestamp),
            wxString::FromUTF8(preview),
            s.message_count);

        auto* entry_menu = new wxMenu;
        entry_menu->Append(ID_MENU_SESSION_OPEN_BASE   + static_cast<int>(i), "Open");
        entry_menu->Append(ID_MENU_SESSION_DELETE_BASE + static_cast<int>(i), "Delete");

        sessions_menu_->AppendSubMenu(entry_menu, label);
        sessions_in_menu_.push_back(s.id);
    }

    if (sessions.size() > k_max_sessions_in_menu) {
        sessions_menu_->AppendSeparator();
        sessions_menu_->Append(wxID_ANY,
            wxString::Format("(%zu more not shown)", sessions.size() - k_max_sessions_in_menu))
            ->Enable(false);
    }
}

void LocusFrame::on_session_open(wxCommandEvent& evt)
{
    size_t i = static_cast<size_t>(evt.GetId() - ID_MENU_SESSION_OPEN_BASE);
    if (i >= sessions_in_menu_.size()) return;
    std::string id = sessions_in_menu_[i];
    try {
        agent_.load_session(id);
        SetStatusText(wxString::Format("Loaded session: %s", id), 0);
    } catch (const std::exception& ex) {
        wxMessageBox(wxString::Format("Failed to load session:\n%s", ex.what()),
                     "Load Session", wxOK | wxICON_ERROR, this);
    }
}

void LocusFrame::on_session_delete(wxCommandEvent& evt)
{
    size_t i = static_cast<size_t>(evt.GetId() - ID_MENU_SESSION_DELETE_BASE);
    if (i >= sessions_in_menu_.size()) return;
    std::string id = sessions_in_menu_[i];
    int answer = wxMessageBox(
        wxString::Format("Delete session '%s'?\nThis cannot be undone.", id),
        "Delete Session", wxYES_NO | wxICON_WARNING, this);
    if (answer != wxYES) return;
    if (agent_.sessions().remove(id)) {
        SetStatusText(wxString::Format("Deleted session: %s", id), 0);
        rebuild_sessions_menu();
    }
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

void LocusFrame::sync_view_menu()
{
    if (view_files_item_) {
        auto& p = aui_.GetPane("sidebar");
        if (p.IsOk()) view_files_item_->Check(p.IsShown());
    }
    if (view_activity_item_) {
        auto& p = aui_.GetPane("activity");
        if (p.IsOk()) view_activity_item_->Check(p.IsShown());
    }
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
        });

    // Center chat panel.
    chat_panel_ = new ChatPanel(this,
        [this](const std::string& msg) {
            // Flush any pending file-watcher events first so the agent's
            // first tool call sees a fresh index (mirrors the CLI's
            // pre-turn drain in main.cpp).
            flush_pending_events();
            agent_.send_message(msg);
        },
        [this]() { show_compaction_dialog(); },
        [this]() { agent_.cancel_turn(); });

    // Slash-command suggestions: CLI-style commands + all registered tools.
    {
        std::vector<SlashItem> items = {
            {"help",     "Show available slash commands",         false},
            {"reset",    "Start a fresh conversation",            false},
            {"compact",  "Open the context compaction dialog",    false},
            {"save",     "Save the current session to disk",      false},
            {"settings", "Open the Settings dialog",              false},
            {"clear",    "Alias for /reset",                      false},
        };
        for (auto* t : agent_.tools().all()) {
            items.push_back({ t->name(), t->description(), true });
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
                rebuild_sessions_menu();
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
    // Stop the watcher pump first so no late tick fires into a
    // half-destructed frame, then flush any pending events synchronously
    // so edits made right before close still get indexed.
    watcher_timer_.Stop();
    try {
        flush_pending_events();
    } catch (const std::exception& ex) {
        spdlog::warn("Final flush on close failed: {}", ex.what());
    }

    // The embedding worker runs on its own thread and fires on_progress
    // callbacks into this frame.  Stop it here — before destruction — so no
    // callback can fire into freed memory.  stop() joins the thread.
    if (auto* ew = workspace_.embedding_worker()) {
        ew->on_progress = nullptr;
        ew->stop();
    }

    // Remove tray icon before destroying frame.
    if (tray_)
        tray_->RemoveIcon();
    tray_.reset();

    evt.Skip();  // proceed with destruction
}

// ---------------------------------------------------------------------------
// File-watcher pump + batching
// ---------------------------------------------------------------------------

namespace {
    constexpr auto k_quiet_period = std::chrono::milliseconds(1500);
    constexpr auto k_hard_cap     = std::chrono::milliseconds(20000);
}

void LocusFrame::on_watcher_timer(wxTimerEvent& /*evt*/)
{
    // Pull any new events from the watcher queue.
    std::vector<FileEvent> drained;
    size_t n = workspace_.file_watcher().drain(drained);

    auto now = std::chrono::steady_clock::now();

    if (n > 0) {
        if (!group_active_) {
            group_start_ = now;
            group_active_ = true;
        }
        last_event_time_ = now;
        pending_events_.insert(pending_events_.end(),
                               std::make_move_iterator(drained.begin()),
                               std::make_move_iterator(drained.end()));
        spdlog::trace("Watcher: +{} events (pending {})", n, pending_events_.size());
    }

    if (!group_active_) return;

    auto idle  = now - last_event_time_;
    auto total = now - group_start_;
    if (idle >= k_quiet_period || total >= k_hard_cap) {
        flush_pending_events();
    }
}

void LocusFrame::flush_pending_events()
{
    if (pending_events_.empty()) {
        group_active_ = false;
        return;
    }

    spdlog::info("File watcher: flushing {} batched event(s)",
                 pending_events_.size());

    // Move into a local so a concurrent append (e.g. if a nested call path
    // could somehow reenter) doesn't see a half-cleared vector. Unlikely
    // here since everything is on the UI thread, but cheap insurance.
    std::vector<FileEvent> batch = std::move(pending_events_);
    pending_events_.clear();
    group_active_ = false;

    try {
        workspace_.indexer().process_events(batch);
    } catch (const std::exception& ex) {
        spdlog::error("process_events failed: {}", ex.what());
    }

    refresh_index_stats();
}

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
    if (auto* pane = evt.GetPane()) {
        if (pane->name == "sidebar" && view_files_item_)
            view_files_item_->Check(false);
        else if (pane->name == "activity" && view_activity_item_)
            view_activity_item_->Check(false);
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

    embedding_done_  = done;
    embedding_total_ = (total > 0 && done >= total) ? 0 : total;
    refresh_ops_status();
}

void LocusFrame::on_agent_indexing_progress(wxThreadEvent& evt)
{
    int done = evt.GetInt();
    int total = static_cast<int>(evt.GetExtraLong());

    indexing_done_  = done;
    indexing_total_ = (total > 0 && done >= total) ? 0 : total;
    refresh_ops_status();
}

void LocusFrame::refresh_ops_status()
{
    wxString out;
    auto append = [&out](const wxString& s) {
        if (!out.empty()) out += "  |  ";
        out += s;
    };

    if (indexing_total_ > 0) {
        double pct = 100.0 * indexing_done_ / indexing_total_;
        append(wxString::Format("indexing %d/%d files %.1f%%",
                                indexing_done_, indexing_total_, pct));
    }
    if (embedding_total_ > 0) {
        double pct = 100.0 * embedding_done_ / embedding_total_;
        append(wxString::Format("embedding %d/%d chunks %.1f%%",
                                embedding_done_, embedding_total_, pct));
    }

    SetStatusText(out, 1);
}

void LocusFrame::on_agent_activity(wxThreadEvent& evt)
{
    auto ev = evt.GetPayload<ActivityEvent>();
    if (activity_panel_) activity_panel_->append(ev);
}

} // namespace locus
