#include "locus_frame.h"
#include "about_dialog.h"
#include "app_icons.h"
#include "locus_accessible.h"
#include "locus_app.h"
#include "manage_sessions_dialog.h"
#include "ui_names.h"
#include "ui_state.h"

#include "../../core/locus_tab.h"
#include "../../core/watcher_pump.h"
#include "../../core/workspace_ui_state.h"
#include "../../index/embedding_worker.h"
#include "../../index/indexer.h"
#include "../../mcp/mcp_manager.h"
#include "../../tools/process_registry.h"

#include <spdlog/spdlog.h>

#include <wx/display.h>
#include <wx/dirdlg.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>

#include <algorithm>

namespace locus {

wxBEGIN_EVENT_TABLE(LocusFrame, wxFrame)
    EVT_CLOSE(LocusFrame::on_close)
    EVT_ICONIZE(LocusFrame::on_iconize)
    EVT_AUI_PANE_CLOSE(LocusFrame::on_aui_pane_close)
wxEND_EVENT_TABLE()

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LocusFrame::LocusFrame(LocusSession& session)
    : wxFrame(nullptr, wxID_ANY,
              wxString::Format("Locus - %s",
                               wxString::FromUTF8(session.workspace().root().string())),
              wxDefaultPosition, wxSize(1200, 800))
    , session_(session)
    , workspace_(session.workspace())
    , mcp_(session.mcp())
{
    SetName(ui_names::kMainFrame);
    SetIcon(gui::app_icon(32));
    gui::apply_locus_accessible_name(this);

    saved_ui_state_ = load_ui_state();

    aui_.SetManagedWindow(this);

    // Menu bar -- hooks routed through LocusFrame methods.
    MenuController::Hooks hooks;
    hooks.on_quit                  = [this] { Close(false); };
    hooks.on_open_workspace_dialog = [this] {
        wxDirDialog dlg(this, "Select workspace folder",
            wxStandardPaths::Get().GetDocumentsDir(),
            wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            std::string path = dlg.GetPath().ToStdString();
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
    hooks.on_settings             = [this] { show_settings_dialog(); };
    hooks.on_compact              = [this] { show_compaction_dialog(); };
    hooks.on_new_tab              = [this] { on_new_tab(); };
    hooks.on_close_active_tab     = [this] { on_close_active_tab(); };
    hooks.on_rename_active_tab    = [this] { on_rename_active_tab(); };
    hooks.on_load_session         = [this](std::string id) { on_open_saved_session(id); };
    hooks.on_rename_session       = [this](std::string id) {
        auto sessions = session_.sessions().list();
        for (const auto& info : sessions) {
            if (info.id == id) {
                wxTextEntryDialog dlg(this, "Rename session",
                    "New title:", wxString::FromUTF8(info.title));
                if (dlg.ShowModal() == wxID_OK) {
                    auto new_title = dlg.GetValue().ToStdString();
                    if (!new_title.empty())
                        session_.sessions().rename(id, new_title);
                }
                break;
            }
        }
    };
    hooks.on_delete_session       = [this](std::string id) { on_delete_saved_session(id); };
    hooks.on_manage_sessions      = [this] { on_manage_sessions(/*cleanup=*/false); };
    hooks.on_cleanup_sessions     = [this] { on_manage_sessions(/*cleanup=*/true); };
    hooks.on_toggle_files_pane    = [this](bool show) {
        if (auto& p = aui_.GetPane("sidebar"); p.IsOk()) { p.Show(show); aui_.Update(); }
    };
    hooks.on_toggle_activity_pane = [this](bool show) {
        if (auto& p = aui_.GetPane("activity"); p.IsOk()) { p.Show(show); aui_.Update(); }
    };
    hooks.on_toggle_terminal_pane = [this](bool show) {
        if (auto& p = aui_.GetPane("terminal"); p.IsOk()) {
            p.Show(show);
            aui_.Update();
        }
        // S5.R -- manual re-open clears the active tab's "user hid" flag so
        // future first-command auto-show works again. Manual hide via View
        // menu sets it (on_aui_pane_close fires for any pane-close path,
        // including the menu).
        if (show && notebook_) {
            if (auto* ui = find_tab_ui(0); ui) (void)ui;  // silence unused
            // Resolve the active tab via notebook selection.
            int sel = notebook_->GetSelection();
            if (sel >= 0) {
                auto* chat = static_cast<ChatPanel*>(notebook_->GetPage(sel));
                for (auto& [tid, ui] : tabs_ui_) {
                    if (ui.chat == chat && ui.tab) {
                        ui.tab->set_terminal_user_hidden(false);
                        break;
                    }
                }
            }
        }
    };
    hooks.on_about = [this] {
        AboutDialog dlg(this);
        dlg.ShowModal();
    };
    hooks.provide_sessions = [this] { return session_.sessions().list(); };
    hooks.provide_open_ids = [this] { return session_.open_session_ids(); };

    menu_ = std::make_unique<MenuController>(this, std::move(hooks));
    menu_->install();

    // -- Add tabs to the LocusSession BEFORE setup_aui_layout, which reads
    // -- session_.active_tab() for the ActivityPanel binding. UI installation
    // -- happens in the second pass below once the notebook widget exists.
    auto wui = load_workspace_ui_state(workspace_.locus_dir());
    int  restore_active = 0;
    bool restored_any   = false;
    if (workspace_.config().sessions.restore_last && !wui.open_tabs.empty()) {
        for (size_t i = 0; i < wui.open_tabs.size(); ++i) {
            const auto& ot = wui.open_tabs[i];
            try {
                int idx = session_.add_tab_for_session(ot.session_id);
                if (idx >= 0) {
                    if (ot.active) restore_active = idx;
                    restored_any = true;
                }
            } catch (const std::exception& ex) {
                spdlog::warn("Tab restore: skipping '{}': {}", ot.session_id, ex.what());
            }
        }
    }
    if (!restored_any) {
        session_.add_tab();
    }
    session_.set_active_tab(
        (restore_active >= 0 && restore_active < session_.tab_count())
            ? restore_active : 0);

    create_status_bar();
    setup_aui_layout();

    // -- Pass 2: install per-tab ChatPanel + WxFrontend now that the notebook
    // -- widget exists. Iterate the LocusSession tabs in order so notebook
    // -- pages line up with tab indices.
    for (int i = 0; i < session_.tab_count(); ++i) {
        auto& tab = session_.tab(i);
        install_tab_ui(tab, wxString::FromUTF8(tab.title().empty() ? "New tab" : tab.title()));
    }
    if (notebook_ && session_.active_index() >= 0
                  && session_.active_index() < static_cast<int>(notebook_->GetPageCount())) {
        notebook_->SetSelection(session_.active_index());
    }

    if (!saved_ui_state_.aui_perspective.empty()) {
        if (aui_.LoadPerspective(
                wxString::FromUTF8(saved_ui_state_.aui_perspective),
                /*update=*/false)) {
            if (menu_) {
                menu_->set_files_pane_visible   (aui_.GetPane("sidebar") .IsShown());
                menu_->set_activity_pane_visible(aui_.GetPane("activity").IsShown());
                menu_->set_terminal_pane_visible(aui_.GetPane("terminal").IsShown());
            }
        }
    }

    aui_.Update();
    apply_saved_window_geometry();

    // S5.R -- bind the terminal panel to the initial active tab's state.
    // Done after AUI perspective load so the pane's hidden/shown state is
    // already settled; set_state itself doesn't touch pane visibility.
    if (terminal_panel_ && session_.tab_count() > 0) {
        terminal_panel_->set_state(&session_.active_tab().terminal_state());
        terminal_panel_->set_first_command_observer([this] {
            // No-op here -- the per-tab observer's lifecycle hooks drive
            // auto-show. The widget observer slot exists for future use.
        });
    }

    tray_ = std::make_unique<LocusTray>(this);

    // -- Workspace-shared bridge for indexer / embedding worker callbacks ---
    shared_bridge_ = std::make_unique<WxFrontend>(this, /*tab_id=*/0);
    workspace_.indexer().on_progress = [this](int done, int total) {
        shared_bridge_->on_indexing_progress(done, total);
    };
    if (auto* ew = workspace_.embedding_worker()) {
        ew->on_progress = [this](int done, int total) {
            shared_bridge_->on_embedding_progress(done, total);
        };
    }

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
    Bind(EVT_AGENT_MODE_CHANGED,        &LocusFrame::on_agent_mode_changed,       this);
    Bind(EVT_AGENT_PLAN_PROPOSED,       &LocusFrame::on_agent_plan_proposed,      this);
    Bind(EVT_AGENT_PLAN_STEP_ADVANCED,  &LocusFrame::on_agent_plan_step_advanced, this);
    Bind(EVT_AGENT_PLAN_COMPLETED,      &LocusFrame::on_agent_plan_completed,     this);
    Bind(EVT_AGENT_AUTO_COMMIT,         &LocusFrame::on_agent_auto_commit,        this);
    Bind(EVT_AGENT_GEN_PROGRESS,        &LocusFrame::on_agent_gen_progress,       this);
    Bind(EVT_AGENT_HISTORY_MSG_ADDED,   &LocusFrame::on_agent_history_msg_added,   this);
    Bind(EVT_AGENT_HISTORY_MSG_DELETED, &LocusFrame::on_agent_history_msg_deleted, this);

    // Notebook events.
    if (notebook_) {
        notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED,
                        &LocusFrame::on_notebook_page_changed, this);
        notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE,
                        &LocusFrame::on_notebook_page_close, this);
        notebook_->Bind(wxEVT_AUINOTEBOOK_TAB_RIGHT_UP,
                        &LocusFrame::on_notebook_tab_right_up, this);
    }

    refresh_index_stats();
    Centre();
    spdlog::info("LocusFrame created with {} tab(s)", session_.tab_count());

    // S5.I -- auto-cleanup at workspace open. Defer to CallAfter so the frame
    // is fully painted before the modal pops; only fires when there are real
    // candidates. The dialog itself is the user's confirm gate -- nothing is
    // deleted without an explicit OK.
    if (workspace_.config().sessions.auto_cleanup_enabled) {
        CallAfter([this] {
            auto open_ids = session_.open_session_ids();
            auto candidates = session_.sessions().cleanup_candidates(
                workspace_.config().sessions.keep_last_count,
                workspace_.config().sessions.delete_after_days,
                open_ids);
            if (candidates.empty()) return;
            on_manage_sessions(/*cleanup=*/true);
        });
    }
}

LocusFrame::~LocusFrame()
{
    detach_from_session();
    aui_.UnInit();
}

void LocusFrame::detach_from_session()
{
    if (session_detached_) return;
    session_detached_ = true;

    if (workspace_.embedding_worker())
        workspace_.embedding_worker()->on_progress = nullptr;
    workspace_.indexer().on_progress = nullptr;

    // S5.R -- detach the terminal panel from any active state before tabs
    // tear down (states are owned by LocusTab).
    if (terminal_panel_) terminal_panel_->set_state(nullptr);

    // Unregister every tab's frontend + process observer from its agent /
    // broker. Order: unsubscribe sinks before the LocusSession tears down
    // each LocusTab (which would destroy the brokers).
    for (auto& [tab_id, ui] : tabs_ui_) {
        if (ui.tab && ui.frontend)
            ui.tab->agent().unregister_frontend(ui.frontend.get());
        if (ui.tab && ui.proc_obs) {
            if (auto* broker = ui.tab->processes().sink_broker())
                broker->remove_sink(ui.proc_obs.get());
        }
    }
}

// ---------------------------------------------------------------------------
// Status bar / AUI layout
// ---------------------------------------------------------------------------

void LocusFrame::create_status_bar()
{
    CreateStatusBar(2);
    const int widths[] = { -1, -3 };
    SetStatusWidths(2, widths);
    SetStatusText("Ready", 0);
    SetStatusText("", 1);
}

void LocusFrame::setup_aui_layout()
{
    file_tree_panel_ = new FileTreePanel(this, workspace_.query(),
        workspace_.root().string(),
        [this](const std::string& path) {
            spdlog::trace("File selected in tree: {}", path);
            SetStatusText(wxString::FromUTF8(path), 0);
        },
        [this](const std::string& path) {
            session_.active_tab().agent().set_attached_context({path, /*preview*/{}});
        });

    notebook_ = new wxAuiNotebook(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxAUI_NB_DEFAULT_STYLE | wxAUI_NB_WINDOWLIST_BUTTON);
    notebook_->SetName(ui_names::kChatNotebook);
    gui::apply_locus_accessible_name(notebook_);

    activity_panel_ = new ActivityPanel(this, session_.active_tab().agent());

    terminal_panel_ = new TerminalPanel(this,
        [this](int bg_id) {
            // S5.R -- route to the active tab's registry. The terminal
            // panel shows the active tab's content, so a kill from its
            // right-click menu always targets that tab's processes.
            if (!notebook_) return;
            int sel = notebook_->GetSelection();
            if (sel < 0) return;
            auto* chat = static_cast<ChatPanel*>(notebook_->GetPage(sel));
            for (auto& [tid, ui] : tabs_ui_) {
                if (ui.chat == chat && ui.tab) {
                    ui.tab->processes().stop(bg_id);
                    break;
                }
            }
        });

    aui_.AddPane(file_tree_panel_, wxAuiPaneInfo()
        .Name("sidebar").Caption("Files")
        .Left().MinSize(180, -1).BestSize(240, -1)
        .CloseButton(true).PinButton(true));

    aui_.AddPane(notebook_, wxAuiPaneInfo()
        .Name("chat").Caption("Chat")
        .CenterPane());

    aui_.AddPane(activity_panel_, wxAuiPaneInfo()
        .Name("activity").Caption("Activity")
        .Right().MinSize(280, -1).BestSize(420, -1)
        .CloseButton(true).PinButton(true));

    aui_.AddPane(terminal_panel_, wxAuiPaneInfo()
        .Name("terminal").Caption("Terminal")
        .Bottom().MinSize(-1, 160).BestSize(-1, 240)
        .CloseButton(true).PinButton(true)
        .Hide());
}

// ---------------------------------------------------------------------------
// Tab UI install / teardown
// ---------------------------------------------------------------------------

void LocusFrame::configure_chat_panel(ChatPanel* chat, LocusTab& tab)
{
    auto* agent_ptr = &tab.agent();

    chat->set_on_detach([agent_ptr]() { agent_ptr->clear_attached_context(); });

    {
        std::vector<std::string> paths;
        for (const auto& f : workspace_.query().list_directory("", 100)) {
            if (!f.is_directory) paths.push_back(f.path);
        }
        chat->set_mention_paths(std::move(paths));
    }
    chat->set_on_mention_attach([agent_ptr](const std::string& path) {
        agent_ptr->set_attached_context({path, /*preview*/{}});
    });

    chat->set_pre_mutation_fetcher(
        [agent_ptr](const std::string& rel_path) -> std::optional<std::string> {
            return agent_ptr->read_current_pre_mutation(rel_path);
        });
    chat->set_diff_options(workspace_.config().chat_show_diffs,
                            workspace_.config().chat_diff_max_lines,
                            workspace_.config().chat_diff_context_lines,
                            workspace_.config().chat_diff_collapse_threshold);
    chat->set_show_per_message_tokens(workspace_.config().ui_show_per_message_tokens);
    chat->set_system_prompt_bubble(agent_ptr->context().system_prompt());
    chat->set_on_delete_message([agent_ptr](int history_id) {
        agent_ptr->delete_message(history_id);
    });

    {
        std::vector<SlashItem> items = {
            {"help",     "Show available slash commands",         false},
            {"compact",  "Open the context compaction dialog",    false},
            {"settings", "Open the Settings dialog",              false},
            {"undo",     "Revert files mutated by the last turn", false},
        };
        for (const auto& c : agent_ptr->slash_dispatcher().complete("")) {
            std::string desc = c.signature.empty()
                ? c.description
                : c.signature + "  -  " + c.description;
            items.push_back({ c.name, std::move(desc), true });
        }
        chat->set_slash_commands(std::move(items));
    }

    chat->set_on_slash_command(
        [this, agent_ptr](const std::string& name, const std::string& rest) -> bool {
            if (name == "compact") { show_compaction_dialog(); return true; }
            if (name == "settings") { show_settings_dialog();  return true; }
            if (name == "help") {
                wxString html =
                    "<span class=\"tool-name\">Slash commands</span><br>"
                    "<span class=\"tool-preview\">"
                    "/help&nbsp;&nbsp;-- show this help<br>"
                    "/compact&nbsp;&nbsp;-- compact context<br>"
                    "/settings&nbsp;&nbsp;-- open settings<br>"
                    "Open a new tab (Ctrl+T) to start a fresh conversation."
                    "</span>";
                int idx = notebook_index_for_chat(
                    static_cast<ChatPanel*>(notebook_->GetPage(notebook_->GetSelection())));
                if (idx >= 0) {
                    auto* page = static_cast<ChatPanel*>(notebook_->GetPage(idx));
                    page->append_system_note(html);
                }
                (void)agent_ptr;
                return true;
            }
            (void)rest;
            return false;
        });
}

LocusFrame::TabUi& LocusFrame::install_tab_ui(LocusTab& tab, const wxString& tab_title)
{
    auto* chat = new ChatPanel(notebook_,
        [this, agent_ptr = &tab.agent()](const std::string& msg) {
            workspace_.watcher_pump().flush_now();
            refresh_index_stats();
            agent_ptr->send_message(msg);
        },
        [this]() { show_compaction_dialog(); },
        [this, agent_ptr = &tab.agent()]() { agent_ptr->cancel_turn(); },
        [this, agent_ptr = &tab.agent()]() {
            std::string summary = agent_ptr->undo_turn();
            wxString html = "<pre>" + wxString::FromUTF8(summary) + "</pre>";
            // Append into the chat page that fired this -- look it up by agent.
            for (auto& [tid, ui] : tabs_ui_) {
                if (ui.tab && &ui.tab->agent() == agent_ptr) {
                    ui.chat->append_system_note(html);
                    break;
                }
            }
        },
        [agent_ptr = &tab.agent()](AgentMode m) { agent_ptr->set_mode(m); },
        [agent_ptr = &tab.agent()](const std::string& decision) {
            if      (decision == "approve") agent_ptr->approve_plan();
            else if (decision == "reject")  agent_ptr->reject_plan();
        });

    configure_chat_panel(chat, tab);

    notebook_->AddPage(chat, tab_title, /*select=*/false);

    TabUi ui;
    ui.tab_id   = tab.tab_id();
    ui.tab      = &tab;
    ui.chat     = chat;
    ui.frontend = std::make_unique<WxFrontend>(this, tab.tab_id());
    ui.proc_obs = std::make_unique<TabProcessObserver>(this, tab.tab_id());

    auto& slot = tabs_ui_[tab.tab_id()] = std::move(ui);
    // Register the frontend AFTER inserting into the map -- on_history_message
    // events may fire immediately.
    tab.agent().register_frontend(slot.frontend.get());
    // S5.R -- subscribe the per-tab process observer to the tab's broker.
    // The tab's TerminalPanelState is already subscribed (LocusTab ctor);
    // this is the second sink the multi-subscriber broker accepts.
    if (auto* broker = tab.processes().sink_broker())
        broker->add_sink(slot.proc_obs.get());

    // Restore-from-disk render path: if this tab was loaded from a saved
    // session BEFORE the UI was installed, the agent's history already holds
    // those messages but the chat WebView is empty.  Paint them now.
    if (!tab.session_id().empty() && tab.agent().history().size() > 1) {
        slot.chat->render_loaded_history(tab.agent().history());
    }
    return slot;
}

void LocusFrame::teardown_tab_ui(int nb_index)
{
    if (nb_index < 0 || !notebook_) return;
    if (nb_index >= static_cast<int>(notebook_->GetPageCount())) return;

    auto* chat = static_cast<ChatPanel*>(notebook_->GetPage(nb_index));
    int   tab_id = 0;
    for (auto& [tid, ui] : tabs_ui_) {
        if (ui.chat == chat) { tab_id = tid; break; }
    }
    if (tab_id == 0) {
        notebook_->RemovePage(nb_index);
        chat->Destroy();
        return;
    }

    auto it = tabs_ui_.find(tab_id);
    if (it != tabs_ui_.end()) {
        if (it->second.tab && it->second.frontend)
            it->second.tab->agent().unregister_frontend(it->second.frontend.get());
        // S5.R -- unsubscribe the observer BEFORE LocusTab teardown so a
        // worker thread firing one last event can't land on a freed obs.
        if (it->second.tab && it->second.proc_obs) {
            if (auto* broker = it->second.tab->processes().sink_broker())
                broker->remove_sink(it->second.proc_obs.get());
        }
        // S5.R -- if the active terminal state was this tab's, detach the
        // widget first; LocusSession::close_tab destroys the LocusTab next
        // and with it the TerminalPanelState we'd be pointing at.
        if (terminal_panel_ && it->second.tab) {
            if (terminal_panel_) {
                terminal_panel_->set_state(nullptr);
            }
        }
        tabs_ui_.erase(it);
    }

    notebook_->RemovePage(nb_index);
    chat->Destroy();
}

LocusFrame::TabUi* LocusFrame::find_tab_ui(int tab_id)
{
    auto it = tabs_ui_.find(tab_id);
    return it == tabs_ui_.end() ? nullptr : &it->second;
}

int LocusFrame::notebook_index_for_tab_id(int tab_id) const
{
    if (!notebook_) return -1;
    auto it = tabs_ui_.find(tab_id);
    if (it == tabs_ui_.end()) return -1;
    return notebook_index_for_chat(it->second.chat);
}

int LocusFrame::notebook_index_for_chat(const ChatPanel* chat) const
{
    if (!notebook_) return -1;
    for (size_t i = 0; i < notebook_->GetPageCount(); ++i) {
        if (notebook_->GetPage(i) == chat) return static_cast<int>(i);
    }
    return -1;
}

LocusFrame::TabUi& LocusFrame::active_tab_ui()
{
    int sel = notebook_->GetSelection();
    auto* chat = static_cast<ChatPanel*>(notebook_->GetPage(sel));
    for (auto& [tid, ui] : tabs_ui_) {
        if (ui.chat == chat) return ui;
    }
    // Should never happen -- fall back to the first entry.
    return tabs_ui_.begin()->second;
}

LocusTab& LocusFrame::active_tab() { return *active_tab_ui().tab; }

wxString LocusFrame::compose_tab_label(const TabUi& ui) const
{
    wxString base = ui.tab ? wxString::FromUTF8(ui.tab->title()) : wxString("(empty)");
    wxString prefix;
    // S5.R -- single `*` for any of: agent turn busy, awaiting tool decision,
    // bg processes running. Conflated on purpose; if real usage distinguishes
    // them, split into two glyphs later.
    if (ui.busy || ui.busy_processes > 0) prefix += "* ";
    if (ui.awaiting_decision)             prefix += "! ";
    if (ui.ctx_over_auto)                 prefix += "[!] ";
    else if (ui.ctx_over_warn)            prefix += "[.] ";
    return prefix + base;
}

void LocusFrame::refresh_tab_title(int tab_id)
{
    auto* ui = find_tab_ui(tab_id);
    if (!ui || !notebook_) return;
    int idx = notebook_index_for_tab_id(tab_id);
    if (idx < 0) return;
    notebook_->SetPageText(idx, compose_tab_label(*ui));
}

// ---------------------------------------------------------------------------
// Tab interactions
// ---------------------------------------------------------------------------

void LocusFrame::on_new_tab()
{
    int idx = session_.add_tab();
    auto& tab = session_.tab(idx);
    install_tab_ui(tab, "New tab");
    session_.set_active_tab(idx);
    if (notebook_) notebook_->SetSelection(idx);
}

void LocusFrame::on_close_active_tab()
{
    if (!notebook_ || notebook_->GetPageCount() == 0) return;
    int sel = notebook_->GetSelection();
    if (sel < 0) return;
    teardown_tab_ui(sel);
    session_.close_tab(sel);

    // If close_tab auto-opened a fresh empty tab (last tab closed), install
    // its UI now and select it.
    while (static_cast<int>(notebook_->GetPageCount()) < session_.tab_count()) {
        int new_idx = static_cast<int>(notebook_->GetPageCount());
        auto& tab = session_.tab(new_idx);
        install_tab_ui(tab, "New tab");
    }
    int active = session_.active_index();
    if (notebook_->GetPageCount() > 0 &&
        active >= 0 && active < static_cast<int>(notebook_->GetPageCount()))
        notebook_->SetSelection(active);
}

void LocusFrame::on_rename_active_tab()
{
    auto& tab = active_tab();
    wxTextEntryDialog dlg(this, "Rename tab",
        "New title:", wxString::FromUTF8(tab.title()));
    if (dlg.ShowModal() == wxID_OK) {
        auto new_title = dlg.GetValue().ToStdString();
        if (!new_title.empty()) {
            tab.set_title(new_title);
            refresh_tab_title(tab.tab_id());
        }
    }
}

void LocusFrame::on_notebook_page_changed(wxAuiNotebookEvent& evt)
{
    int sel = evt.GetSelection();
    if (sel < 0 || !notebook_) { evt.Skip(); return; }
    session_.set_active_tab(sel);
    // S5.R -- swap the terminal panel's content to the newly-active tab.
    if (terminal_panel_) {
        auto* chat = static_cast<ChatPanel*>(notebook_->GetPage(sel));
        TerminalPanelState* state = nullptr;
        for (auto& [tid, ui] : tabs_ui_) {
            if (ui.chat == chat && ui.tab) {
                state = &ui.tab->terminal_state();
                break;
            }
        }
        terminal_panel_->set_state(state);
    }
    if (menu_) menu_->rebuild_sessions_menu(session_.sessions().list());
    evt.Skip();
}

void LocusFrame::on_notebook_page_close(wxAuiNotebookEvent& evt)
{
    // wxAuiNotebook fires this just before removing the page. Veto + run
    // through our own close path so the LocusSession stays in sync.
    int sel = evt.GetSelection();
    evt.Veto();
    if (sel < 0) return;
    teardown_tab_ui(sel);
    session_.close_tab(sel);
    while (static_cast<int>(notebook_->GetPageCount()) < session_.tab_count()) {
        int new_idx = static_cast<int>(notebook_->GetPageCount());
        auto& tab = session_.tab(new_idx);
        install_tab_ui(tab, "New tab");
    }
    int active = session_.active_index();
    if (notebook_->GetPageCount() > 0 &&
        active >= 0 && active < static_cast<int>(notebook_->GetPageCount()))
        notebook_->SetSelection(active);
}

void LocusFrame::on_notebook_tab_right_up(wxAuiNotebookEvent& evt)
{
    int sel = evt.GetSelection();
    if (sel < 0) return;
    auto* chat = static_cast<ChatPanel*>(notebook_->GetPage(sel));
    int tab_id = 0;
    for (auto& [tid, ui] : tabs_ui_) {
        if (ui.chat == chat) { tab_id = tid; break; }
    }
    if (tab_id == 0) return;

    enum {
        MID_RENAME = 9100, MID_CLOSE, MID_CLOSE_OTHERS, MID_CLOSE_RIGHT, MID_DELETE
    };
    wxMenu menu;
    menu.Append(MID_RENAME,       "Rename...");
    menu.Append(MID_CLOSE,        "Close");
    menu.Append(MID_CLOSE_OTHERS, "Close Others");
    menu.Append(MID_CLOSE_RIGHT,  "Close to the Right");
    menu.AppendSeparator();
    menu.Append(MID_DELETE,       "Delete Session...");
    int picked = GetPopupMenuSelectionFromUser(menu);
    switch (picked) {
        case MID_RENAME: {
            wxTextEntryDialog dlg(this, "Rename tab",
                "New title:", notebook_->GetPageText(sel));
            if (dlg.ShowModal() == wxID_OK) {
                auto new_title = dlg.GetValue().ToStdString();
                if (!new_title.empty()) {
                    tabs_ui_[tab_id].tab->set_title(new_title);
                    refresh_tab_title(tab_id);
                }
            }
            break;
        }
        case MID_CLOSE: {
            teardown_tab_ui(sel);
            session_.close_tab(sel);
            while (static_cast<int>(notebook_->GetPageCount()) < session_.tab_count()) {
                int new_idx = static_cast<int>(notebook_->GetPageCount());
                auto& tab = session_.tab(new_idx);
                install_tab_ui(tab, "New tab");
            }
            break;
        }
        case MID_CLOSE_OTHERS: {
            for (int i = static_cast<int>(notebook_->GetPageCount()) - 1; i >= 0; --i) {
                if (i == sel) continue;
                teardown_tab_ui(i);
            }
            // Close-others on LocusSession: iterate from highest-index, skip sel.
            for (int i = session_.tab_count() - 1; i >= 0; --i) {
                if (i == sel) continue;
                session_.close_tab(i);
            }
            while (static_cast<int>(notebook_->GetPageCount()) < session_.tab_count()) {
                int new_idx = static_cast<int>(notebook_->GetPageCount());
                auto& tab = session_.tab(new_idx);
                install_tab_ui(tab, "New tab");
            }
            break;
        }
        case MID_CLOSE_RIGHT: {
            for (int i = static_cast<int>(notebook_->GetPageCount()) - 1; i > sel; --i) {
                teardown_tab_ui(i);
                session_.close_tab(i);
            }
            break;
        }
        case MID_DELETE: {
            auto& tab = *tabs_ui_[tab_id].tab;
            int answer = wxMessageBox(
                wxString::Format("Delete session '%s' and any archived history? "
                                  "This cannot be undone.",
                                  wxString::FromUTF8(tab.title())),
                "Delete Session", wxYES_NO | wxICON_WARNING, this);
            if (answer != wxYES) break;
            teardown_tab_ui(sel);
            session_.delete_tab(sel);
            while (static_cast<int>(notebook_->GetPageCount()) < session_.tab_count()) {
                int new_idx = static_cast<int>(notebook_->GetPageCount());
                auto& t = session_.tab(new_idx);
                install_tab_ui(t, "New tab");
            }
            break;
        }
    }
    if (menu_) menu_->rebuild_sessions_menu(session_.sessions().list());
}

// ---------------------------------------------------------------------------
// Session menu actions
// ---------------------------------------------------------------------------

void LocusFrame::on_open_saved_session(const std::string& session_id)
{
    // If the session is already open, just activate that tab.
    int existing = session_.find_tab_by_session(session_id);
    if (existing >= 0) {
        if (notebook_) notebook_->SetSelection(existing);
        return;
    }

    try {
        int idx = session_.add_tab_for_session(session_id);
        if (idx < 0) return;
        auto& tab = session_.tab(idx);
        install_tab_ui(tab, wxString::FromUTF8(tab.title()));
        session_.set_active_tab(idx);
        if (notebook_) notebook_->SetSelection(idx);
    } catch (const std::exception& ex) {
        wxMessageBox(wxString::Format("Failed to open session:\n%s", ex.what()),
                     "Open Session", wxOK | wxICON_ERROR, this);
    }
}

void LocusFrame::on_delete_saved_session(const std::string& session_id)
{
    auto sessions = session_.sessions().list();
    std::string title = session_id;
    for (const auto& s : sessions) {
        if (s.id == session_id) { title = s.title.empty() ? s.id : s.title; break; }
    }
    int answer = wxMessageBox(
        wxString::Format("Delete session '%s' and any archived history? "
                         "This cannot be undone.", wxString::FromUTF8(title)),
        "Delete Session", wxYES_NO | wxICON_WARNING, this);
    if (answer != wxYES) return;
    session_.sessions().remove(session_id);
    if (menu_) menu_->rebuild_sessions_menu(session_.sessions().list());
}

void LocusFrame::on_manage_sessions(bool prefilter_cleanup)
{
    auto all = session_.sessions().list();
    auto open_ids = session_.open_session_ids();
    std::vector<std::string> preselect;
    if (prefilter_cleanup) {
        preselect = session_.sessions().cleanup_candidates(
            workspace_.config().sessions.keep_last_count,
            workspace_.config().sessions.delete_after_days,
            open_ids);
        if (preselect.empty()) {
            wxMessageBox("No sessions match the current cleanup criteria.",
                         "Clean Up Old Sessions", wxOK | wxICON_INFORMATION, this);
            return;
        }
    }

    ManageSessionsDialog::Hooks hooks;
    hooks.on_open = [this](const std::string& id) { on_open_saved_session(id); };
    hooks.on_rename = [this](const std::string& id, const std::string& current) -> std::string {
        wxTextEntryDialog dlg(this, "Rename session", "New title:",
                               wxString::FromUTF8(current));
        if (dlg.ShowModal() != wxID_OK) return {};
        auto t = dlg.GetValue().ToStdString();
        if (t.empty()) return {};
        session_.sessions().rename(id, t);
        return t;
    };
    hooks.on_delete = [this](const std::string& id) {
        session_.sessions().remove(id);
    };

    ManageSessionsDialog dlg(this, std::move(all), std::move(open_ids),
                              std::move(preselect),
                              prefilter_cleanup ? ManageSessionsDialog::Mode::cleanup
                                                : ManageSessionsDialog::Mode::normal,
                              std::move(hooks));
    dlg.ShowModal();
    if (menu_) menu_->rebuild_sessions_menu(session_.sessions().list());
}

// ---------------------------------------------------------------------------
// Compaction / Settings
// ---------------------------------------------------------------------------

void LocusFrame::show_compaction_dialog()
{
    auto& agent = session_.active_tab().agent();
    int used  = static_cast<int>(agent.history().estimate_tokens());
    int limit = agent.context_limit();

    CompactionDialog dlg(this, used, limit, agent.history(),
                          workspace_.config().compaction);
    if (dlg.ShowModal() == wxID_OK) {
        auto choice = dlg.result();
        if (choice.made) {
            agent.run_compaction(choice.selection, /*target=*/0,
                                   choice.custom_instructions);
            SetStatusText("Context compaction queued", 0);
        }
    }
}

void LocusFrame::show_settings_dialog()
{
    SettingsDialog dlg(this, workspace_.config(),
                        session_.active_tab().agent().tools(), mcp_);
    if (dlg.ShowModal() == wxID_OK && dlg.config_changed()) {
        workspace_.save_config();
        SetStatusText("Settings saved", 0);

        if (dlg.llm_changed()) {
            wxMessageBox(
                "LLM settings changed.\nRestart Locus to apply new endpoint/model settings.",
                "Settings", wxOK | wxICON_INFORMATION, this);
        }

        if (dlg.semantic_changed()) {
            workspace_.disable_semantic_search();
            if (workspace_.config().semantic_search_enabled) {
                if (!workspace_.enable_semantic_search()) {
                    wxMessageBox(
                        "Could not enable semantic search.\n"
                        "Check that the GGUF model file exists in the models/ folder.",
                        "Semantic Search", wxOK | wxICON_WARNING, this);
                }
            }
        }
        for (auto& [tid, ui] : tabs_ui_) {
            ui.chat->set_diff_options(workspace_.config().chat_show_diffs,
                                       workspace_.config().chat_diff_max_lines,
                                       workspace_.config().chat_diff_context_lines,
                                       workspace_.config().chat_diff_collapse_threshold);
        }
        spdlog::info("Settings saved to config.json");
    }
}

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
    if (s.window_width >= 320 && s.window_height >= 240) {
        SetSize(s.window_width, s.window_height);
    }
    if (s.window_x != -1 && s.window_y != -1) {
        wxRect want(s.window_x, s.window_y,
                    s.window_width  >= 0 ? s.window_width  : 800,
                    s.window_height >= 0 ? s.window_height : 600);
        bool on_screen = false;
        for (unsigned i = 0; i < wxDisplay::GetCount(); ++i) {
            if (wxDisplay(i).GetGeometry().Intersects(want)) {
                on_screen = true; break;
            }
        }
        if (on_screen) SetPosition(wxPoint(s.window_x, s.window_y));
    }
    if (s.window_maximized) Maximize(true);
}

void LocusFrame::on_close(wxCloseEvent& evt)
{
    // S5.I -- capture open_tabs + active tab into workspace UI state.
    {
        WorkspaceUiState wui;
        // Persist tabs first so all have session ids.
        session_.persist_all_tabs();
        for (int i = 0; i < session_.tab_count(); ++i) {
            auto& t = session_.tab(i);
            if (t.session_id().empty()) continue;  // skip empty
            WorkspaceUiState::OpenTab ot;
            ot.session_id = t.session_id();
            ot.title      = t.title();
            ot.active     = (i == session_.active_index());
            wui.open_tabs.push_back(ot);
        }
        save_workspace_ui_state(workspace_.locus_dir(), wui);
    }

    {
        UiState s;
        s.window_maximized = IsMaximized();
        wxRect r = GetRect();
        s.window_x      = r.GetX();
        s.window_y      = r.GetY();
        s.window_width  = r.GetWidth();
        s.window_height = r.GetHeight();
        s.aui_perspective = aui_.SavePerspective().ToStdString();
        save_ui_state(s);
    }

    if (auto* ew = workspace_.embedding_worker()) {
        ew->on_progress = nullptr;
        ew->stop();
    }

    if (tray_) tray_->RemoveIcon();
    tray_.reset();

    detach_from_session();
    evt.Skip();
}

void LocusFrame::on_iconize(wxIconizeEvent& evt)
{
    if (evt.IsIconized()) Hide();
    evt.Skip();
}

void LocusFrame::on_aui_pane_close(wxAuiManagerEvent& evt)
{
    if (auto* pane = evt.GetPane(); pane && menu_) {
        if (pane->name == "sidebar")
            menu_->set_files_pane_visible(false);
        else if (pane->name == "activity")
            menu_->set_activity_pane_visible(false);
        else if (pane->name == "terminal") {
            menu_->set_terminal_pane_visible(false);
            // S5.R -- manual hide of the terminal pane wins: mark the
            // currently active tab so the next command in that tab does
            // NOT re-pop the pane. Manual re-open via View > Terminal
            // clears the flag (handled in on_toggle_terminal_pane).
            if (notebook_) {
                int sel = notebook_->GetSelection();
                if (sel >= 0) {
                    auto* chat = static_cast<ChatPanel*>(notebook_->GetPage(sel));
                    for (auto& [tid, ui] : tabs_ui_) {
                        if (ui.chat == chat && ui.tab) {
                            ui.tab->set_terminal_user_hidden(true);
                            break;
                        }
                    }
                }
            }
        }
    }
    evt.Skip();
}

// ---------------------------------------------------------------------------
// Agent thread events -- routed by tab_id (evt.GetId())
// ---------------------------------------------------------------------------

void LocusFrame::on_agent_turn_start(wxThreadEvent& evt)
{
    int tab_id = evt.GetId();
    SetStatusText("Agent working...", 0);
    if (tray_) tray_->set_state(LocusTray::State::active);
    if (auto* ui = find_tab_ui(tab_id)) {
        ui->busy = true;
        refresh_tab_title(tab_id);
        ui->chat->on_turn_start();
    }
}

void LocusFrame::on_agent_token(wxThreadEvent& evt)
{
    if (auto* ui = find_tab_ui(evt.GetId()))
        ui->chat->on_token(evt.GetString());
}

void LocusFrame::on_agent_reasoning_token(wxThreadEvent& evt)
{
    if (auto* ui = find_tab_ui(evt.GetId()))
        ui->chat->on_reasoning_token(evt.GetString());
}

void LocusFrame::on_agent_tool_pending(wxThreadEvent& evt)
{
    int tab_id = evt.GetId();
    auto* ui = find_tab_ui(tab_id);
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
        refresh_tab_title(tab_id);

        auto* agent_ptr = &ui->tab->agent();
        if (tool == "ask_user") {
            std::string question = args.value("question", "");
            AskUserDialog dlg(this, question);
            if (dlg.ShowModal() == wxID_OK) {
                nlohmann::json modified = args;
                modified["response"] = dlg.response();
                agent_ptr->tool_decision(call_id, ToolDecision::modify, modified);
            } else {
                agent_ptr->tool_decision(call_id, ToolDecision::reject, {});
            }
        } else {
            ToolApprovalDialog::run(this, call_id, tool, args, preview,
                safety_warnings,
                [agent_ptr](const std::string& cid, ToolDecision d,
                            const nlohmann::json& modified) {
                    agent_ptr->tool_decision(cid, d, modified);
                });
        }
        ui->awaiting_decision = false;
        refresh_tab_title(tab_id);
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse tool_pending payload: {}", ex.what());
    }
}

void LocusFrame::on_agent_tool_result(wxThreadEvent& evt)
{
    auto* ui = find_tab_ui(evt.GetId());
    if (!ui) return;
    try {
        auto payload = nlohmann::json::parse(evt.GetString().ToUTF8().data());
        wxString call_id = wxString::FromUTF8(payload.value("call_id", ""));
        wxString display = wxString::FromUTF8(payload.value("display", ""));
        bool success = payload.value("success", true);
        ui->chat->on_tool_result(call_id, display, success);
    } catch (...) {}
}

void LocusFrame::on_agent_turn_complete(wxThreadEvent& evt)
{
    int tab_id = evt.GetId();
    SetStatusText("Ready", 0);
    if (tray_) tray_->set_state(LocusTray::State::idle);
    if (auto* ui = find_tab_ui(tab_id)) {
        ui->busy = false;
        refresh_tab_title(tab_id);
        ui->chat->on_turn_complete();
    }
    // S5.I -- a tab's title might have just been autoderived from its first
    // user message; refresh the notebook label.
    if (auto* ui = find_tab_ui(tab_id); ui && ui->tab)
        refresh_tab_title(tab_id);
}

void LocusFrame::on_agent_context_meter(wxThreadEvent& evt)
{
    auto* ui = find_tab_ui(evt.GetId());
    if (!ui) return;
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
        } catch (...) {}
    }
    ui->chat->set_context_meter(used, limit, prompt, completion, reserve);

    int effective = limit - reserve;
    if (effective <= 0) effective = limit > 0 ? limit : 1;
    double ratio = static_cast<double>(used) / static_cast<double>(effective);
    bool was_warn = ui->ctx_over_warn;
    bool was_auto = ui->ctx_over_auto;
    ui->ctx_over_warn = ratio >= workspace_.config().compaction.warn_threshold;
    ui->ctx_over_auto = ratio >= workspace_.config().compaction.auto_threshold;
    if (was_warn != ui->ctx_over_warn || was_auto != ui->ctx_over_auto)
        refresh_tab_title(evt.GetId());
}

void LocusFrame::on_agent_compaction(wxThreadEvent& /*evt*/)
{
    show_compaction_dialog();
}

void LocusFrame::on_agent_session_reset(wxThreadEvent& evt)
{
    SetStatusText("Conversation reset", 0);
    if (auto* ui = find_tab_ui(evt.GetId())) {
        ui->chat->on_session_reset();
        ui->chat->set_system_prompt_bubble(ui->tab->agent().context().system_prompt());
    }
    if (activity_panel_) activity_panel_->clear();
}

void LocusFrame::on_agent_error(wxThreadEvent& evt)
{
    wxString msg = evt.GetString();
    SetStatusText("Error: " + msg, 0);
    if (tray_) tray_->set_state(LocusTray::State::error);
    if (auto* ui = find_tab_ui(evt.GetId())) ui->chat->on_error(msg);
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
    if (tray_) {
        const bool agent_busy = std::any_of(
            tabs_ui_.begin(), tabs_ui_.end(),
            [](const auto& entry) { return entry.second.busy; });
        if (!agent_busy) {
            tray_->set_state(total > 0 && done < total
                ? LocusTray::State::indexing
                : LocusTray::State::idle);
        }
    }
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
    // Activity is workspace-shared; route to the single panel regardless of
    // source tab.
    auto ev = evt.GetPayload<ActivityEvent>();
    if (activity_panel_) activity_panel_->append(ev);
}

void LocusFrame::on_agent_attached_context(wxThreadEvent& evt)
{
    if (auto* ui = find_tab_ui(evt.GetId()))
        ui->chat->set_attached_chip(evt.GetString());
}

void LocusFrame::on_agent_mode_changed(wxThreadEvent& evt)
{
    if (auto* ui = find_tab_ui(evt.GetId())) {
        auto mode = static_cast<AgentMode>(evt.GetInt());
        ui->chat->on_mode_changed(mode);
    }
}

void LocusFrame::on_agent_plan_proposed(wxThreadEvent& evt)
{
    if (auto* ui = find_tab_ui(evt.GetId()))
        ui->chat->on_plan_proposed(evt.GetString());
}

void LocusFrame::on_agent_plan_step_advanced(wxThreadEvent& evt)
{
    auto* ui = find_tab_ui(evt.GetId());
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
}

void LocusFrame::on_agent_plan_completed(wxThreadEvent& evt)
{
    auto* ui = find_tab_ui(evt.GetId());
    if (!ui) return;
    try {
        auto j = nlohmann::json::parse(evt.GetString().utf8_string());
        wxString plan_id = wxString::FromUTF8(j.value("plan_id", ""));
        bool     success = j.value("success", false);
        ui->chat->on_plan_completed(plan_id, success);
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse plan_completed payload: {}", ex.what());
    }
}

void LocusFrame::on_agent_auto_commit(wxThreadEvent& evt)
{
    auto* ui = find_tab_ui(evt.GetId());
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
}

void LocusFrame::on_agent_gen_progress(wxThreadEvent& evt)
{
    auto* ui = find_tab_ui(evt.GetId());
    if (!ui) return;
    int chars      = evt.GetInt();
    int est_tokens = static_cast<int>(evt.GetExtraLong());
    ui->chat->set_generation_progress(chars, est_tokens);
}

void LocusFrame::on_agent_history_msg_added(wxThreadEvent& evt)
{
    auto* ui = find_tab_ui(evt.GetId());
    if (!ui) return;
    int history_id = evt.GetInt();
    long packed    = evt.GetExtraLong();
    MessageRole role = static_cast<MessageRole>(packed & 0xFF);
    bool deletable   = (packed & 0x100) != 0;
    ui->chat->on_history_message_added(history_id, role, deletable);
    // First user message may have just landed -- autoderived title.
    if (role == MessageRole::user)
        refresh_tab_title(evt.GetId());
}

void LocusFrame::on_agent_history_msg_deleted(wxThreadEvent& evt)
{
    if (auto* ui = find_tab_ui(evt.GetId()))
        ui->chat->on_history_message_deleted(evt.GetInt());
}

// ---------------------------------------------------------------------------
// S5.R -- per-tab process observer (lifecycle only)
// ---------------------------------------------------------------------------

void LocusFrame::TabProcessObserver::on_bg_started(int /*id*/, const std::string& /*cmd*/)
{
    LocusFrame* frame = frame_;
    int tab_id = tab_id_;
    if (!frame) return;
    frame->CallAfter([frame, tab_id] { frame->on_tab_bg_started(tab_id); });
}

void LocusFrame::TabProcessObserver::on_bg_exited(int /*id*/, int /*exit_code*/, bool /*killed*/)
{
    LocusFrame* frame = frame_;
    int tab_id = tab_id_;
    if (!frame) return;
    frame->CallAfter([frame, tab_id] { frame->on_tab_bg_exited(tab_id); });
}

void LocusFrame::TabProcessObserver::on_sync_started(const std::string& /*cmd*/)
{
    LocusFrame* frame = frame_;
    int tab_id = tab_id_;
    if (!frame) return;
    frame->CallAfter([frame, tab_id] { frame->on_tab_sync_started(tab_id); });
}

void LocusFrame::maybe_auto_show_terminal_for(int tab_id)
{
    auto* ui = find_tab_ui(tab_id);
    if (!ui || !ui->tab) return;

    // Resolve the active tab via notebook selection rather than
    // session_.active_index() so a not-yet-propagated tab change can't
    // misroute the auto-show decision.
    int active_tab_id = 0;
    if (notebook_) {
        int sel = notebook_->GetSelection();
        if (sel >= 0) {
            auto* active = static_cast<ChatPanel*>(notebook_->GetPage(sel));
            for (auto& [tid, u] : tabs_ui_) {
                if (u.chat == active) { active_tab_id = tid; break; }
            }
        }
    }

    bool pane_hidden = true;
    if (auto& p = aui_.GetPane("terminal"); p.IsOk()) pane_hidden = !p.IsShown();

    if (should_auto_show_terminal(tab_id, active_tab_id, pane_hidden,
                                    ui->tab->terminal_user_hidden())) {
        if (auto& p = aui_.GetPane("terminal"); p.IsOk()) {
            p.Show(true);
            aui_.Update();
            if (menu_) menu_->set_terminal_pane_visible(true);
        }
    }
}

void LocusFrame::on_tab_bg_started(int tab_id)
{
    auto* ui = find_tab_ui(tab_id);
    if (!ui) return;
    ++ui->busy_processes;
    refresh_tab_title(tab_id);
    maybe_auto_show_terminal_for(tab_id);
}

void LocusFrame::on_tab_bg_exited(int tab_id)
{
    auto* ui = find_tab_ui(tab_id);
    if (!ui) return;
    if (ui->busy_processes > 0) --ui->busy_processes;
    refresh_tab_title(tab_id);
}

void LocusFrame::on_tab_sync_started(int tab_id)
{
    // Sync runs DON'T move the busy-process counter (sync is short-lived;
    // the agent is already showing busy via on_turn_start). We only want
    // the auto-show check.
    maybe_auto_show_terminal_for(tab_id);
}

} // namespace locus
