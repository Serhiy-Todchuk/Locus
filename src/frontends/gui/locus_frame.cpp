#include "locus_frame.h"
#include "locus_accessible.h"
#include "locus_app.h"
#include "ui_names.h"
#include "ui_state.h"

#include "../../core/watcher_pump.h"
#include "../../index/embedding_worker.h"
#include "../../index/indexer.h"

#include <spdlog/spdlog.h>

#include <wx/aboutdlg.h>
#include <wx/display.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>

namespace locus {

wxBEGIN_EVENT_TABLE(LocusFrame, wxFrame)
    EVT_CLOSE(LocusFrame::on_close)
    EVT_ICONIZE(LocusFrame::on_iconize)
    EVT_AUI_PANE_CLOSE(LocusFrame::on_aui_pane_close)
wxEND_EVENT_TABLE()

LocusFrame::LocusFrame(AgentCore& agent, Workspace& workspace, McpManager* mcp)
    : wxFrame(nullptr, wxID_ANY,
              wxString::Format("Locus - %s",
                               wxString::FromUTF8(workspace.root().string())),
              wxDefaultPosition, wxSize(1200, 800))
    , agent_(agent)
    , workspace_(workspace)
    , mcp_(mcp)
{
    SetName(ui_names::kMainFrame);
    gui::apply_locus_accessible_name(this);

    // Load any previously-saved window layout. Applied later in two phases:
    // AUI perspective after panes are added, geometry after aui_.Update().
    saved_ui_state_ = load_ui_state();

    // AUI manager
    aui_.SetManagedWindow(this);

    // Menu bar -- owned by MenuController; LocusFrame supplies action hooks.
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
    hooks.on_open_workspace_folder = [this] {
        wxString url = wxString::FromUTF8(workspace_.root().string());
        if (!wxLaunchDefaultApplication(url)) {
            wxMessageBox(wxString::Format(
                "Could not open %s. Open it manually.", url),
                "Locus", wxOK | wxICON_WARNING, this);
        }
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
    hooks.on_toggle_terminal_pane = [this](bool show) { toggle_pane("terminal", show); };
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

    // Restore the user's saved AUI pane layout, if any. Must run after
    // setup_aui_layout() so every pane exists in the manager, and before
    // aui_.Update() so the loaded perspective is what gets laid out.
    if (!saved_ui_state_.aui_perspective.empty()) {
        if (aui_.LoadPerspective(
                wxString::FromUTF8(saved_ui_state_.aui_perspective),
                /*update=*/false)) {
            spdlog::info("Restored AUI perspective from ~/.locus/ui_state.json");
            // Sync the View-menu checkboxes with the actual pane visibility
            // (the saved perspective may have closed panes we'd otherwise
            // show as checked).
            if (menu_) {
                menu_->set_files_pane_visible   (aui_.GetPane("sidebar") .IsShown());
                menu_->set_activity_pane_visible(aui_.GetPane("activity").IsShown());
                menu_->set_terminal_pane_visible(aui_.GetPane("terminal").IsShown());
            }
        } else {
            spdlog::warn("Failed to load saved AUI perspective; using defaults");
        }
    }

    aui_.Update();

    // Restore the saved window geometry. Done after AUI Update so the wx
    // sizing pass doesn't fight us, and after construction so child panels
    // are fully built. The on-display sanity check below guards against a
    // monitor going away between sessions (laptop undocked, etc.).
    apply_saved_window_geometry();

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
    // S4.D plan-mode events.
    Bind(EVT_AGENT_MODE_CHANGED,        &LocusFrame::on_agent_mode_changed,       this);
    Bind(EVT_AGENT_PLAN_PROPOSED,       &LocusFrame::on_agent_plan_proposed,      this);
    Bind(EVT_AGENT_PLAN_STEP_ADVANCED,  &LocusFrame::on_agent_plan_step_advanced, this);
    Bind(EVT_AGENT_PLAN_COMPLETED,      &LocusFrame::on_agent_plan_completed,     this);
    // S4.L auto-commit footer chip.
    Bind(EVT_AGENT_AUTO_COMMIT,         &LocusFrame::on_agent_auto_commit,        this);
    // S4.F live generation-progress chip.
    Bind(EVT_AGENT_GEN_PROGRESS,        &LocusFrame::on_agent_gen_progress,       this);

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

    // S5.B -- detach the terminal sink so process_registry / run_command
    // can't reach a panel that's about to be destroyed.
    if (terminal_panel_) terminal_panel_->detach();

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
    // Negative values are proportional -- left takes 1 share, right takes 3.
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
        // Right-click -> "Attach to context" hands the workspace-relative path
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
            // a system note in chat -- the same string the /undo command emits.
            std::string summary = agent_.undo_turn();
            wxString html = "<pre>" + wxString::FromUTF8(summary) + "</pre>";
            chat_panel_->append_system_note(html);
        },
        // S4.D mode switcher click -> AgentCore queues the change.
        [this](AgentMode m) { agent_.set_mode(m); },
        // S4.D plan Approve / Reject from the chat bubble.
        [this](const std::string& decision) {
            if      (decision == "approve") agent_.approve_plan();
            else if (decision == "reject")  agent_.reject_plan();
        });

    // Chip "✕" -> detach the pinned file from the conversation context.
    chat_panel_->set_on_detach([this]() {
        agent_.clear_attached_context();
    });

    // S4.V -- seed the @-mention autocomplete from the indexed file list.
    // Captured once at workspace open; new files added mid-session won't
    // appear in the popup until the next workspace open. The index is
    // already up-to-date for typed-out paths, so missing-from-popup paths
    // still attach correctly at submit time if the user types the path
    // verbatim.
    {
        std::vector<std::string> paths;
        for (const auto& f : workspace_.query().list_directory("", 100)) {
            if (!f.is_directory) paths.push_back(f.path);
        }
        chat_panel_->set_mention_paths(std::move(paths));
    }
    chat_panel_->set_on_mention_attach([this](const std::string& path) {
        agent_.set_attached_context({path, /*preview*/{}});
    });

    // S5.C -- diff rendering hooks. The fetcher reads the pre-mutation
    // snapshot for write_file / delete_file diffs from the current turn's
    // checkpoint; the options carry the workspace toggle + line cap.
    chat_panel_->set_pre_mutation_fetcher(
        [this](const std::string& rel_path) -> std::optional<std::string> {
            return agent_.read_current_pre_mutation(rel_path);
        });
    chat_panel_->set_diff_options(workspace_.config().chat_show_diffs,
                                   workspace_.config().chat_diff_max_lines,
                                   workspace_.config().chat_diff_context_lines,
                                   workspace_.config().chat_diff_collapse_threshold);

    // S5.D -- per-message token chips.
    chat_panel_->set_show_per_message_tokens(
        workspace_.config().ui_show_per_message_tokens);

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
                    "/help&nbsp;&nbsp;-- show this help<br>"
                    "/reset | /clear&nbsp;&nbsp;-- reset conversation<br>"
                    "/compact&nbsp;&nbsp;-- compact context<br>"
                    "/save&nbsp;&nbsp;-- save session<br>"
                    "/settings&nbsp;&nbsp;-- open settings<br>"
                    "/&lt;tool&gt; &lt;args&gt;&nbsp;&nbsp;-- type a tool name"
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

    // S5.Z #6 -- the tool-approval surface is a self-dismissing modal
    // (ToolApprovalDialog) constructed per pending call inside
    // on_agent_tool_pending; no permanent panel / AUI pane is owned by the
    // frame. The dialog blocks the agent thread (already condvar-blocked in
    // ToolDispatcher) until the user decides; X / Alt+F4 routes to a reject
    // so the dispatcher never deadlocks.

    // Right detail panel -- Activity log (S2.2).
    activity_panel_ = new ActivityPanel(this, agent_);

    // Terminal panel (S5.B). Hidden by default; View menu toggles it.
    terminal_panel_ = new TerminalPanel(this,
        workspace_.process_sink(),
        workspace_.processes());

    // Add panes.
    aui_.AddPane(file_tree_panel_, wxAuiPaneInfo()
        .Name("sidebar").Caption("Files")
        .Left().MinSize(180, -1).BestSize(240, -1)
        .CloseButton(true).PinButton(true));

    aui_.AddPane(chat_panel_, wxAuiPaneInfo()
        .Name("chat").Caption("Chat")
        .CenterPane());

    // S5.Z #6 -- approval pane removed; ToolApprovalDialog (modal) replaces it.

    aui_.AddPane(activity_panel_, wxAuiPaneInfo()
        .Name("activity").Caption("Activity")
        .Right().MinSize(280, -1).BestSize(420, -1)
        .CloseButton(true).PinButton(true));

    aui_.AddPane(terminal_panel_, wxAuiPaneInfo()
        .Name("terminal").Caption("Terminal")
        .Bottom().MinSize(-1, 160).BestSize(-1, 240)
        .CloseButton(true).PinButton(true)
        .Hide());  // S5.B -- hidden by default, View menu toggles
}

// ---------------------------------------------------------------------------
// Compaction dialog
// ---------------------------------------------------------------------------

void LocusFrame::show_compaction_dialog()
{
    int used  = static_cast<int>(agent_.history().estimate_tokens());
    int limit = agent_.context_limit();

    CompactionDialog dlg(this, used, limit, agent_.history(),
                          workspace_.config().compaction);
    if (dlg.ShowModal() == wxID_OK) {
        auto choice = dlg.result();
        if (choice.made) {
            agent_.run_compaction(choice.selection, /*target=*/0,
                                   choice.custom_instructions);
            SetStatusText("Context compaction queued", 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Settings dialog
// ---------------------------------------------------------------------------

void LocusFrame::show_settings_dialog()
{
    SettingsDialog dlg(this, workspace_.config(), agent_.tools(), mcp_);
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

        // S5.C -- pick up any change to the chat-diff toggles immediately;
        // no restart required, the chat panel just reads them at next
        // tool_result.
        chat_panel_->set_diff_options(workspace_.config().chat_show_diffs,
                                       workspace_.config().chat_diff_max_lines,
                                       workspace_.config().chat_diff_context_lines,
                                       workspace_.config().chat_diff_collapse_threshold);

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

void LocusFrame::apply_saved_window_geometry()
{
    const auto& s = saved_ui_state_;

    // Apply size first (so position math, if needed, sees the right rect).
    // Only apply when both dimensions look sane; otherwise the wxSize(1200,800)
    // ctor default stands.
    if (s.window_width >= 320 && s.window_height >= 240) {
        SetSize(s.window_width, s.window_height);
    }

    // Apply position only when the rect lands on a currently-attached
    // display. This guards against unplugging the monitor the window was
    // last on -- without the check, the frame can spawn off-screen.
    if (s.window_x != -1 && s.window_y != -1) {
        wxRect want(s.window_x, s.window_y,
                    s.window_width  >= 0 ? s.window_width  : 800,
                    s.window_height >= 0 ? s.window_height : 600);
        bool on_screen = false;
        for (unsigned i = 0; i < wxDisplay::GetCount(); ++i) {
            if (wxDisplay(i).GetGeometry().Intersects(want)) {
                on_screen = true;
                break;
            }
        }
        if (on_screen) {
            SetPosition(wxPoint(s.window_x, s.window_y));
        } else {
            spdlog::info("Saved window position is off-screen; centering on default display");
        }
    }

    if (s.window_maximized) {
        Maximize(true);
    }
}

void LocusFrame::on_close(wxCloseEvent& evt)
{
    // Persist current window + AUI layout for the next launch. Done first
    // so a later detach / shutdown failure can't lose the state. Errors
    // are logged inside save_ui_state.
    {
        UiState s;
        s.window_maximized = IsMaximized();
        // When maximized, GetRect() returns the maximized extents; we still
        // save them and re-apply Maximize() on next launch. The underlying
        // restored geometry is lost (wx has no portable accessor for it),
        // but the user just sees a maximized window again -- the common case.
        wxRect r = GetRect();
        s.window_x      = r.GetX();
        s.window_y      = r.GetY();
        s.window_width  = r.GetWidth();
        s.window_height = r.GetHeight();
        s.aui_perspective = aui_.SavePerspective().ToStdString();
        save_ui_state(s);
    }

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
    // User clicked the pane's X -- mirror the change in the View menu.
    if (auto* pane = evt.GetPane(); pane && menu_) {
        if (pane->name == "sidebar")
            menu_->set_files_pane_visible(false);
        else if (pane->name == "activity")
            menu_->set_activity_pane_visible(false);
        else if (pane->name == "terminal")
            menu_->set_terminal_pane_visible(false);
    }
    evt.Skip();
}

// ---------------------------------------------------------------------------
// Agent thread events -- UI thread handlers
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
        // ToUTF8() preserves the original UTF-8 bytes the agent thread
        // produced. ToStdString() applies the narrow C locale (CP-1252 on
        // Windows) and silently mangles characters outside Latin-1 -- e.g. an
        // em-dash inside an LLM-emitted write_file payload becomes '?', the
        // JSON parse fails, the approval UI never appears, and the agent
        // thread blocks forever waiting for a tool_decision.
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
        // S4-followup: dispatcher fires on_tool_call_pending for every tool
        // call (including auto-approved). The flag tells us whether to also
        // pop the approval UI; chat rendering happens unconditionally so the
        // result has a header to attach to.
        bool needs_approval = payload.value("needs_approval", true);

        // Show tool call inline in chat (always, regardless of policy).
        // S5.C -- args is also forwarded so on_tool_result can render an
        // inline diff for successful edit_file / write_file / delete_file
        // calls without the args being threaded through a second event.
        chat_panel_->on_tool_pending(
            wxString::FromUTF8(call_id),
            wxString::FromUTF8(tool),
            wxString::FromUTF8(preview),
            args);

        if (!needs_approval) {
            // Auto-approved -- the dispatcher will not wait. Don't pop the
            // approval panel; the chat header above is the user-visible
            // record of the call.
            return;
        }

        if (tool == "ask_user") {
            // Pop-up modal -- disappears immediately after answer/cancel.
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
            // S5.Z #6 -- modal approval dialog. Blocks until the user
            // decides; the agent thread is already condvar-blocked in
            // ToolDispatcher waiting on tool_decision, so wedging the UI
            // here doesn't lose any background progress. ToolApprovalDialog
            // funnels every exit path (button / Esc / Enter / X / Alt+F4)
            // through agent_.tool_decision so the dispatcher always wakes.
            ToolApprovalDialog::run(this, call_id, tool, args, preview,
                safety_warnings,
                [this](const std::string& cid, ToolDecision d,
                       const nlohmann::json& modified) {
                    agent_.tool_decision(cid, d, modified);
                });
        }
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse tool_pending payload: {}", ex.what());
    }
}

void LocusFrame::on_agent_tool_result(wxThreadEvent& evt)
{
    // S5.Z #6 -- no docked approval panel to hide; the modal dialog already
    // tore itself down via EndModal in send_decision when the user decided.

    try {
        // Same UTF-8 round-trip discipline as on_agent_tool_pending above --
        // tool results commonly contain non-ASCII (UI display strings, file
        // contents, error messages) that would otherwise be corrupted by the
        // narrow C locale and silently dropped.
        auto payload = nlohmann::json::parse(evt.GetString().ToUTF8().data());
        wxString call_id = wxString::FromUTF8(payload.value("call_id", ""));
        wxString display = wxString::FromUTF8(payload.value("display", ""));
        bool success = payload.value("success", true);
        chat_panel_->on_tool_result(call_id, display, success);
    } catch (...) {}
}

void LocusFrame::on_agent_turn_complete(wxThreadEvent& /*evt*/)
{
    SetStatusText("Ready", 0);
    if (tray_) tray_->set_state(LocusTray::State::idle);

    // S5.Z #6 -- no approval panel to dismiss; ToolApprovalDialog cleans up
    // its own modal when the user decides.

    chat_panel_->on_turn_complete();
}

void LocusFrame::on_agent_context_meter(wxThreadEvent& evt)
{
    int used  = evt.GetInt();
    int limit = static_cast<int>(evt.GetExtraLong());
    int prompt = 0, completion = 0, reserve = 0;
    auto payload_str = evt.GetString().ToUTF8();
    if (payload_str.length() > 0) {
        try {
            auto j = nlohmann::json::parse(std::string(payload_str.data(), payload_str.length()));
            prompt     = j.value("prompt", 0);
            completion = j.value("completion", 0);
            reserve    = j.value("reserve", 0);
        } catch (...) { /* legacy event without payload -- ignore */ }
    }
    chat_panel_->set_context_meter(used, limit, prompt, completion, reserve);
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

    // Batch finished -- refresh the file tree footer counters AND repopulate
    // the tree itself so files added/removed externally show up without
    // requiring a workspace re-open. The tree currently rebuilds from
    // scratch on each batch (state like expanded folders + selection is
    // lost); a tree-diff version is doable but isn't worth the complexity
    // until users complain.
    if (total > 0 && done >= total) {
        refresh_index_stats();
        if (file_tree_panel_) file_tree_panel_->rebuild();
    }
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

// -- S4.D plan-mode event dispatch -----------------------------------------

void LocusFrame::on_agent_mode_changed(wxThreadEvent& evt)
{
    if (!chat_panel_) return;
    auto mode = static_cast<AgentMode>(evt.GetInt());
    chat_panel_->on_mode_changed(mode);
}

void LocusFrame::on_agent_plan_proposed(wxThreadEvent& evt)
{
    if (!chat_panel_) return;
    chat_panel_->on_plan_proposed(evt.GetString());
}

void LocusFrame::on_agent_plan_step_advanced(wxThreadEvent& evt)
{
    if (!chat_panel_) return;
    try {
        auto j = nlohmann::json::parse(evt.GetString().utf8_string());
        wxString plan_id = wxString::FromUTF8(j.value("plan_id", ""));
        int      step    = j.value("step_idx", 0);
        wxString status  = wxString::FromUTF8(j.value("status", "pending"));
        wxString notes   = wxString::FromUTF8(j.value("notes", ""));
        chat_panel_->on_plan_step_advanced(plan_id, step, status, notes);
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse plan_step_advanced payload: {}", ex.what());
    }
}

void LocusFrame::on_agent_plan_completed(wxThreadEvent& evt)
{
    if (!chat_panel_) return;
    try {
        auto j = nlohmann::json::parse(evt.GetString().utf8_string());
        wxString plan_id = wxString::FromUTF8(j.value("plan_id", ""));
        bool     success = j.value("success", false);
        chat_panel_->on_plan_completed(plan_id, success);
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse plan_completed payload: {}", ex.what());
    }
}

void LocusFrame::on_agent_auto_commit(wxThreadEvent& evt)
{
    if (!chat_panel_) return;
    try {
        auto j = nlohmann::json::parse(evt.GetString().utf8_string());
        wxString sha     = wxString::FromUTF8(j.value("short_sha", ""));
        wxString branch  = wxString::FromUTF8(j.value("branch", ""));
        wxString subject = wxString::FromUTF8(j.value("subject", ""));
        chat_panel_->on_auto_commit(sha, branch, subject);
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse auto_commit payload: {}", ex.what());
    }
}

void LocusFrame::on_agent_gen_progress(wxThreadEvent& evt)
{
    if (!chat_panel_) return;
    int chars      = evt.GetInt();
    int est_tokens = static_cast<int>(evt.GetExtraLong());
    chat_panel_->set_generation_progress(chars, est_tokens);
}

} // namespace locus
