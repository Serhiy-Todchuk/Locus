#include "locus_frame.h"
#include "about_dialog.h"
#include "agent_event_router.h"
#include "app_icons.h"
#include "locus_accessible.h"
#include "locus_app.h"
#include "manage_sessions_dialog.h"
#include "new_tab_button_tab_art.h"
#include "notification_sounds.h"
#include "endpoint_tooltip.h"
#include "ui_names.h"
#include "ui_state.h"

#include "../../llm/endpoint_profile.h"
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
    // S6.18 G.2 -- AUI perspective lives per-workspace now (carried on
    // WorkspaceUiState alongside the open-tabs list). The load below
    // hydrates open_tabs for the restore path AND the perspective for
    // the AUI restore further down; reuse it for both.
    {
        auto wui = load_workspace_ui_state(workspace_.locus_dir());
        saved_ui_state_.aui_perspective = std::move(wui.aui_perspective);
    }

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
    hooks.on_toggle_memory_bank_pane = [this](bool show) {
        if (auto& p = aui_.GetPane("memory_bank"); p.IsOk()) { p.Show(show); aui_.Update(); }
        if (show && memory_bank_panel_) memory_bank_panel_->refresh();
    };
    hooks.on_toggle_find_in_chat = [this]() {
        if (!notebook_) return;
        int sel = notebook_->GetSelection();
        if (sel < 0) return;
        auto* chat = static_cast<ChatPanel*>(notebook_->GetPage(sel));
        if (chat) chat->toggle_find_bar();
    };
    hooks.on_hide_to_tray = [this]() { hide_to_tray(); };
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

    // Append the non-closable "+" pseudo-tab AFTER all real tabs so it
    // sits rightmost. install_tab_ui from this point on InsertPages just
    // before it, keeping it pinned to the end.
    new_tab_placeholder_ = install_new_tab_button(notebook_, [this] {
        // Bail out if we're in the middle of closing a tab -- the
        // PAGE_CHANGING that lands on the placeholder is wxAuiNotebook's
        // automatic re-selection after RemovePage, not a user click on +.
        if (suppress_placeholder_trigger_) return;
        on_new_tab();
    });

    if (notebook_ && session_.active_index() >= 0
                  && session_.active_index() < chat_page_count()) {
        notebook_->SetSelection(session_.active_index());
    }

    if (!saved_ui_state_.aui_perspective.empty()) {
        if (aui_.LoadPerspective(
                wxString::FromUTF8(saved_ui_state_.aui_perspective),
                /*update=*/false)) {
            if (menu_) {
                menu_->set_files_pane_visible   (aui_.GetPane("sidebar")    .IsShown());
                menu_->set_activity_pane_visible(aui_.GetPane("activity")   .IsShown());
                menu_->set_terminal_pane_visible(aui_.GetPane("terminal")   .IsShown());
                menu_->set_memory_bank_pane_visible(aui_.GetPane("memory_bank").IsShown());
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

    // Bind agent thread events. Routed through AgentEventRouter so the 27
    // handlers + their helpers live in agent_event_router.cpp instead of
    // inflating this file. Events are still posted to the frame (via
    // WxFrontend), but the bind's receiver is the router instance.
    router_ = std::make_unique<AgentEventRouter>(*this);
    auto* r = router_.get();
    Bind(EVT_AGENT_TURN_START,         &AgentEventRouter::on_agent_turn_start,         r);
    Bind(EVT_AGENT_TOKEN,              &AgentEventRouter::on_agent_token,              r);
    Bind(EVT_AGENT_REASONING_TOKEN,    &AgentEventRouter::on_agent_reasoning_token,    r);
    Bind(EVT_AGENT_TOOL_PENDING,       &AgentEventRouter::on_agent_tool_pending,       r);
    Bind(EVT_AGENT_TOOL_RESULT,        &AgentEventRouter::on_agent_tool_result,        r);
    Bind(EVT_AGENT_TURN_COMPLETE,      &AgentEventRouter::on_agent_turn_complete,      r);
    Bind(EVT_AGENT_CONTEXT_METER,      &AgentEventRouter::on_agent_context_meter,      r);
    Bind(EVT_AGENT_COMPACTION,         &AgentEventRouter::on_agent_compaction,         r);
    Bind(EVT_AGENT_COMPACTION_ARCHIVED,&AgentEventRouter::on_agent_compaction_archived,r);
    Bind(EVT_AGENT_SESSION_RESET,      &AgentEventRouter::on_agent_session_reset,      r);
    Bind(EVT_AGENT_ERROR,              &AgentEventRouter::on_agent_error,              r);
    Bind(EVT_AGENT_EMBEDDING_PROGRESS, &AgentEventRouter::on_agent_embedding_progress, r);
    Bind(EVT_AGENT_INDEXING_PROGRESS,  &AgentEventRouter::on_agent_indexing_progress,  r);
    Bind(EVT_AGENT_ACTIVITY,           &AgentEventRouter::on_agent_activity,           r);
    Bind(EVT_AGENT_ACTIVITY_UPDATED,   &AgentEventRouter::on_agent_activity_updated,   r);
    Bind(EVT_AGENT_ATTACHED_CONTEXT,   &AgentEventRouter::on_agent_attached_context,   r);
    Bind(EVT_AGENT_MODE_CHANGED,       &AgentEventRouter::on_agent_mode_changed,       r);
    Bind(EVT_AGENT_PLAN_PROPOSED,      &AgentEventRouter::on_agent_plan_proposed,      r);
    Bind(EVT_AGENT_PLAN_STEP_ADVANCED, &AgentEventRouter::on_agent_plan_step_advanced, r);
    Bind(EVT_AGENT_PLAN_COMPLETED,     &AgentEventRouter::on_agent_plan_completed,     r);
    Bind(EVT_AGENT_AUTO_COMMIT,        &AgentEventRouter::on_agent_auto_commit,        r);
    Bind(EVT_AGENT_GEN_PROGRESS,       &AgentEventRouter::on_agent_gen_progress,       r);
    Bind(EVT_AGENT_HISTORY_MSG_ADDED,  &AgentEventRouter::on_agent_history_msg_added,  r);
    Bind(EVT_AGENT_HISTORY_MSG_DELETED,&AgentEventRouter::on_agent_history_msg_deleted,r);
    Bind(EVT_AGENT_PRESET_CHANGED,     &AgentEventRouter::on_agent_preset_changed,     r);
    Bind(EVT_AGENT_ROUND_PROGRESS,     &AgentEventRouter::on_agent_round_progress,     r);
    Bind(EVT_AGENT_LLM_RETRY,          &AgentEventRouter::on_agent_llm_retry,          r);
    Bind(EVT_AGENT_UNVERIFIED_SUCCESS, &AgentEventRouter::on_agent_unverified_success, r);
    Bind(EVT_AGENT_WATCHDOG_TRIPPED,   &AgentEventRouter::on_agent_watchdog_tripped,   r);
    Bind(EVT_AGENT_WATCHDOG_CLEARED,   &AgentEventRouter::on_agent_watchdog_cleared,   r);
    Bind(EVT_AGENT_ENDPOINT_CHANGED,   &AgentEventRouter::on_agent_endpoint_changed,   r);

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
    update_memory_bank_menu_state();
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
    // Field 0 (flex 1): general status messages ("Ready", "Agent working...",
    //                   "Auto-compact enabled", etc.) -- written by the
    //                   existing one-line SetStatusText(..., 0) sites.
    // Field 1 (flex 3): ops_status_.compose() string (indexer / embedder
    //                   progress).
    // Field 2 (fixed):  current plan progress for the active tab (footer-
    //                   declutter pass: moved out of the chat footer chip).
    // Field 3 (fixed):  last auto-commit short SHA + branch for the active
    //                   tab (also moved out of the chat footer chip).
    // refresh_active_tab_footer_status() pushes fields 2+3 from the active
    // tab's ChatFooterChips on plan/commit events and on tab switch.
    CreateStatusBar(4);
    const int widths[] = { -1, -3, 280, 220 };
    SetStatusWidths(4, widths);
    SetStatusText("Ready", 0);
    SetStatusText("", 1);
    SetStatusText("", 2);
    SetStatusText("", 3);
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
    // The "+" placeholder is installed in the ctor AFTER the initial tabs
    // are added so it lands rightmost; see ctor body.

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
        },
        [this](int bg_id, const std::string& line) -> bool {
            // S5.Z task 4 -- forward Enter-submitted stdin to the active
            // tab's bg process. Same routing as the kill handler.
            if (!notebook_) return false;
            int sel = notebook_->GetSelection();
            if (sel < 0) return false;
            auto* chat = static_cast<ChatPanel*>(notebook_->GetPage(sel));
            for (auto& [tid, ui] : tabs_ui_) {
                if (ui.chat == chat && ui.tab) {
                    return ui.tab->processes().write_stdin(bg_id, line);
                }
            }
            return false;
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

    // S5.K -- Memory Bank panel. Default hidden; toggled via View > Memory Bank
    // (Ctrl+M). When the workspace has no MemoryStore (capability off), the
    // panel still constructs in a disabled state and the menu item is greyed.
    memory_bank_panel_ = new MemoryBankPanel(this, workspace_.memory());
    aui_.AddPane(memory_bank_panel_, wxAuiPaneInfo()
        .Name("memory_bank").Caption("Memory Bank")
        .Right().MinSize(360, -1).BestSize(520, -1)
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
    chat->set_diff_options(workspace_.config().chat.show_diffs,
                            workspace_.config().chat.diff_max_lines,
                            workspace_.config().chat.diff_context_lines,
                            workspace_.config().chat.diff_collapse_threshold);
    chat->set_show_per_message_tokens(workspace_.config().chat.show_per_message_tokens);
    chat->set_auto_compact_state(workspace_.config().compaction.auto_enabled);
    chat->set_on_auto_compact_toggle([this](bool enabled) {
        on_auto_compact_toggled(enabled);
    });
    chat->set_system_prompt_bubble(agent_ptr->context().system_prompt());
    chat->set_on_delete_message([agent_ptr](int history_id) {
        agent_ptr->delete_message(history_id);
    });
    chat->set_on_permission_preset_pick(
        [agent_ptr](std::optional<tools::PermissionPreset> picked) {
            if (picked) agent_ptr->set_runtime_permission_preset(*picked);
            else        agent_ptr->clear_runtime_permission_preset();
        });

    // S6.16 -- populate the endpoint picker from the global store + wire the
    // pick / sentinel callbacks.
    {
        EndpointProfileStore store;
        store.load();
        std::vector<std::string> names;
        for (const auto& p : store.list())
            if (!p.name.empty()) names.push_back(p.name);
        std::string active = agent_ptr->active_endpoint_name();
        if (active.empty()) active = store.active();
        chat->set_endpoints(names, active);
        if (const auto* prof = store.find(active))
            chat->set_endpoint_tooltip(gui::endpoint_chip_tooltip(*prof));
    }
    chat->set_on_endpoint_pick([agent_ptr](const std::string& name) {
        agent_ptr->request_endpoint_switch(name);
    });
    chat->set_on_open_endpoint_settings([this]() {
        show_settings_dialog(ui_names::kSettingsTabEndpoints);
    });
    // S6.13 follow-up -- Commit-now click cancels the in-flight LLM stream
    // and injects a "Stop reasoning, commit to a tool call now" steering
    // message via the existing request_commit_now() core API.
    chat->set_on_commit_now([agent_ptr]() { agent_ptr->request_commit_now(); });

    // S5.Z task 6 -- resync the compactions chip from disk so a loaded
    // session shows its historic count immediately (the live increment path
    // only catches new compactions). 0 keeps the chip hidden.
    {
        std::string sid = tab.session_id();
        int n = agent_ptr->compacted_archive_count(sid);
        // S6.18 C.3 -- no-op total is only tracked in memory for the
        // current session; a resync from disk doesn't recover it, so the
        // chip just shows the historic count without the no-op suffix.
        // The live increment path (on_compaction_archived) supplies the
        // no-op count for subsequent compactions in this run.
        auto archive_dir = workspace_.root() / ".locus" / "sessions" / sid;
        chat->set_compacted_count(n, 0,
            wxString::FromUTF8(archive_dir.string()));
    }

    {
        std::vector<SlashItem> items = {
            {"help",      "Show available slash commands",            false},
            {"compact",   "Open the context compaction dialog",       false},
            {"settings",  "Open the Settings dialog",                 false},
            {"undo",      "Revert files mutated by the last turn",    false},
            {"breakdown", "Per-message token table for this session", false},
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

    // Insert just before the "+" placeholder so it stays rightmost. If the
    // placeholder hasn't been installed yet (initial-tabs pass), this falls
    // through to a plain AddPage.
    if (new_tab_placeholder_) {
        int placeholder_idx = -1;
        for (int i = 0; i < static_cast<int>(notebook_->GetPageCount()); ++i) {
            if (notebook_->GetPage(i) == new_tab_placeholder_) {
                placeholder_idx = i; break;
            }
        }
        if (placeholder_idx >= 0)
            notebook_->InsertPage(placeholder_idx, chat, tab_title, /*select=*/false);
        else
            notebook_->AddPage(chat, tab_title, /*select=*/false);
    } else {
        notebook_->AddPage(chat, tab_title, /*select=*/false);
    }

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
        slot.chat->render_loaded_history(tab.agent().history(),
                                         &tab.agent().tools());
    }
    return slot;
}

void LocusFrame::teardown_tab_ui(int nb_index)
{
    if (nb_index < 0 || !notebook_) return;
    if (nb_index >= static_cast<int>(notebook_->GetPageCount())) return;
    if (is_placeholder_index(nb_index)) return;  // never tear down the "+" tab

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

void LocusFrame::refresh_active_tab_footer_status()
{
    // Footer-declutter pass: plan + commit chips moved out of the chat
    // footer into status-bar fields 2 + 3. Called from the per-tab
    // plan/auto-commit event paths AND from on_notebook_page_changed so
    // a tab switch re-renders the strings.
    if (tabs_ui_.empty() || !notebook_) {
        SetStatusText("", 2);
        SetStatusText("", 3);
        return;
    }
    int sel = notebook_->GetSelection();
    if (sel < 0 || is_placeholder_index(sel)) {
        SetStatusText("", 2);
        SetStatusText("", 3);
        return;
    }
    auto& ui = active_tab_ui();
    if (!ui.chat) {
        SetStatusText("", 2);
        SetStatusText("", 3);
        return;
    }
    SetStatusText(ui.chat->plan_status_text(),   2);
    SetStatusText(ui.chat->commit_status_text(), 3);
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

wxString LocusFrame::compose_tab_tooltip(const TabUi& ui) const
{
    wxString base = ui.tab ? wxString::FromUTF8(ui.tab->title()) : wxString("(empty)");
    wxString out = base;

    auto add_line = [&out](const wxString& line) {
        out += "\n";
        out += line;
    };

    if (ui.busy)
        add_line("* Agent turn in progress");
    if (ui.busy_processes > 0)
        add_line(wxString::Format("* %d background process%s running",
                                  ui.busy_processes,
                                  ui.busy_processes == 1 ? "" : "es"));
    if (ui.awaiting_decision)
        add_line("! Waiting for tool approval");
    if (ui.ctx_over_auto)
        add_line("[!] Context limit reached - compaction needed before next turn");
    else if (ui.ctx_over_warn)
        add_line("[.] Context approaching limit");

    return out;
}

void LocusFrame::refresh_tab_title(int tab_id)
{
    auto* ui = find_tab_ui(tab_id);
    if (!ui || !notebook_) return;
    int idx = notebook_index_for_tab_id(tab_id);
    if (idx < 0) return;
    notebook_->SetPageText(idx, compose_tab_label(*ui));
    notebook_->SetPageToolTip(idx, compose_tab_tooltip(*ui));
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
    if (!notebook_ || chat_page_count() == 0) return;
    int sel = notebook_->GetSelection();
    if (sel < 0 || is_placeholder_index(sel)) return;
    suppress_placeholder_trigger_ = true;
    teardown_tab_ui(sel);
    session_.close_tab(sel);

    // If close_tab auto-opened a fresh empty tab (last tab closed), install
    // its UI now and select it.
    while (chat_page_count() < session_.tab_count()) {
        int new_idx = chat_page_count();
        auto& tab = session_.tab(new_idx);
        install_tab_ui(tab, "New tab");
    }
    int active = session_.active_index();
    if (chat_page_count() > 0 &&
        active >= 0 && active < chat_page_count())
        notebook_->SetSelection(active);
    suppress_placeholder_trigger_ = false;
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
    if (is_placeholder_index(sel)) { evt.Skip(); return; }
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
    // Footer-declutter pass: plan + commit chips read from the active
    // tab's ChatPanel; refresh their status-bar slots on tab switch.
    refresh_active_tab_footer_status();
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
    if (is_placeholder_index(sel)) return;
    suppress_placeholder_trigger_ = true;
    teardown_tab_ui(sel);
    session_.close_tab(sel);
    while (chat_page_count() < session_.tab_count()) {
        int new_idx = chat_page_count();
        auto& tab = session_.tab(new_idx);
        install_tab_ui(tab, "New tab");
    }
    int active = session_.active_index();
    if (chat_page_count() > 0 &&
        active >= 0 && active < chat_page_count())
        notebook_->SetSelection(active);
    suppress_placeholder_trigger_ = false;
}

void LocusFrame::on_notebook_tab_right_up(wxAuiNotebookEvent& evt)
{
    int sel = evt.GetSelection();
    if (sel < 0) return;
    if (is_placeholder_index(sel)) return;
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
            suppress_placeholder_trigger_ = true;
            teardown_tab_ui(sel);
            session_.close_tab(sel);
            while (chat_page_count() < session_.tab_count()) {
                int new_idx = chat_page_count();
                auto& tab = session_.tab(new_idx);
                install_tab_ui(tab, "New tab");
            }
            suppress_placeholder_trigger_ = false;
            break;
        }
        case MID_CLOSE_OTHERS: {
            suppress_placeholder_trigger_ = true;
            for (int i = chat_page_count() - 1; i >= 0; --i) {
                if (i == sel) continue;
                teardown_tab_ui(i);
            }
            // Close-others on LocusSession: iterate from highest-index, skip sel.
            for (int i = session_.tab_count() - 1; i >= 0; --i) {
                if (i == sel) continue;
                session_.close_tab(i);
            }
            while (chat_page_count() < session_.tab_count()) {
                int new_idx = chat_page_count();
                auto& tab = session_.tab(new_idx);
                install_tab_ui(tab, "New tab");
            }
            suppress_placeholder_trigger_ = false;
            break;
        }
        case MID_CLOSE_RIGHT: {
            suppress_placeholder_trigger_ = true;
            for (int i = chat_page_count() - 1; i > sel; --i) {
                teardown_tab_ui(i);
                session_.close_tab(i);
            }
            suppress_placeholder_trigger_ = false;
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
            suppress_placeholder_trigger_ = true;
            teardown_tab_ui(sel);
            session_.delete_tab(sel);
            while (chat_page_count() < session_.tab_count()) {
                int new_idx = chat_page_count();
                auto& t = session_.tab(new_idx);
                install_tab_ui(t, "New tab");
            }
            suppress_placeholder_trigger_ = false;
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

void LocusFrame::on_auto_compact_toggled(bool enabled)
{
    if (workspace_.config().compaction.auto_enabled == enabled) return;
    workspace_.config().compaction.auto_enabled = enabled;
    workspace_.save_config();
    // Mirror the new state into every other tab so all chat footers stay
    // in sync (the originating tab's checkbox already reflects the click).
    for (auto& [tab_id, ui] : tabs_ui_) {
        if (ui.chat) ui.chat->set_auto_compact_state(enabled);
    }
    SetStatusText(enabled ? "Auto-compact enabled" : "Auto-compact disabled", 0);
}

void LocusFrame::show_compaction_dialog()
{
    auto& agent = session_.active_tab().agent();
    int used  = static_cast<int>(agent.history().estimate_tokens());
    int limit = agent.context_limit();

    CompactionDialog dlg(this, used, limit, agent.history(),
                          workspace_.config().compaction);
    if (dlg.ShowModal() == wxID_OK) {
        auto choice = dlg.result();
        if (choice.save_as_default) {
            auto& cc = workspace_.config().compaction;
            cc.layer_drop_redundant_tool_results = choice.selection.drop_redundant_tool_results;
            cc.layer_strip_large_tool_bodies     = choice.selection.strip_large_tool_bodies;
            cc.layer_drop_old_reasoning          = choice.selection.drop_old_reasoning;
            cc.layer_drop_oldest_turns           = choice.selection.drop_oldest_turns;
            cc.layer_llm_summary                 = choice.selection.llm_summary;
            cc.strip_threshold_tokens                = choice.selection.strip_threshold_tokens;
            cc.older_than_turns                      = choice.selection.older_than_turns;
            cc.keep_recent_turns                     = choice.selection.keep_recent_turns;
            cc.summary_max_tokens                    = choice.selection.summary_max_tokens;
            cc.custom_summary_instructions           = choice.custom_instructions;
            workspace_.save_config();
            SetStatusText("Auto-compact defaults saved", 0);
        } else if (choice.made) {
            agent.run_compaction(choice.selection, /*target=*/0,
                                   choice.custom_instructions);
            SetStatusText("Context compaction queued", 0);
        }
    }
}

void LocusFrame::show_settings_dialog(const char* select_tab)
{
    SettingsDialog dlg(this, workspace_.config(),
                        session_.active_tab().agent().tools(), mcp_, select_tab);
    if (dlg.ShowModal() == wxID_OK && dlg.config_changed()) {
        workspace_.save_config();
        SetStatusText("Settings saved", 0);

        // S6.16 -- the global endpoints store changed: re-populate every tab's
        // chat-footer picker and hot-swap each tab to the now-active profile.
        if (dlg.endpoints_changed()) {
            EndpointProfileStore store;
            store.load();
            std::vector<std::string> names;
            for (const auto& p : store.list())
                if (!p.name.empty()) names.push_back(p.name);
            const std::string active = store.active();
            for (auto& [tid, ui] : tabs_ui_) {
                if (ui.chat) {
                    ui.chat->set_endpoints(names, active);
                    if (const auto* prof = store.find(active))
                        ui.chat->set_endpoint_tooltip(gui::endpoint_chip_tooltip(*prof));
                }
            }
            for (int i = 0; i < session_.tab_count(); ++i)
                session_.tab(i).agent().request_endpoint_switch(active);
        }

        // S5.S -- the saved approval map is the new baseline. Clear every
        // tab's runtime preset override (the chat-footer combobox) so the
        // user isn't silently still elevated after explicitly committing a
        // different policy. rebroadcast_permission_preset() repaints the
        // chip on whichever workspace-config preset was just persisted.
        for (int i = 0; i < session_.tab_count(); ++i) {
            auto& tab = session_.tab(i);
            tab.agent().clear_runtime_permission_preset();
            tab.agent().rebroadcast_permission_preset();
        }

        if (dlg.llm_changed()) {
            wxMessageBox(
                "LLM settings changed.\nRestart Locus to apply new endpoint/model settings.",
                "Settings", wxOK | wxICON_INFORMATION, this);
        }

        if (dlg.semantic_changed()) {
            workspace_.disable_semantic_search();
            if (workspace_.config().index.semantic_search_enabled) {
                if (!workspace_.enable_semantic_search()) {
                    wxMessageBox(
                        "Could not enable semantic search.\n"
                        "Check that the GGUF model file exists in the models/ folder.",
                        "Semantic Search", wxOK | wxICON_WARNING, this);
                }
            }
        }
        for (auto& [tid, ui] : tabs_ui_) {
            ui.chat->set_diff_options(workspace_.config().chat.show_diffs,
                                       workspace_.config().chat.diff_max_lines,
                                       workspace_.config().chat.diff_context_lines,
                                       workspace_.config().chat.diff_collapse_threshold);
        }
        update_memory_bank_menu_state();
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
    // S6.18 G.2 -- same struct also carries the AUI perspective so the
    // workspace's docking layout survives across launches without
    // bleeding into other workspaces.
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
        wui.aui_perspective = aui_.SavePerspective().ToStdString();
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
    // Minimize is plain taskbar behaviour -- the window stays on the taskbar
    // and restores like any other window. Hiding into the tray is a separate
    // explicit action (View > Hide to Tray / hide_to_tray()), never a side
    // effect of the minimize button.
    evt.Skip();
}

void LocusFrame::hide_to_tray()
{
    // Drop any minimized state first so a later tray-restore brings back a
    // normal (non-iconized) window instead of an invisible minimized one.
    if (IsIconized()) Iconize(false);
    Hide();
}

void LocusFrame::on_aui_pane_close(wxAuiManagerEvent& evt)
{
    if (auto* pane = evt.GetPane(); pane && menu_) {
        if (pane->name == "sidebar")
            menu_->set_files_pane_visible(false);
        else if (pane->name == "activity")
            menu_->set_activity_pane_visible(false);
        else if (pane->name == "memory_bank")
            menu_->set_memory_bank_pane_visible(false);
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


void LocusFrame::refresh_ops_status()
{
    SetStatusText(ops_status_.compose(), 1);
}

void LocusFrame::update_memory_bank_menu_state()
{
    if (!menu_) return;
    bool can_use = workspace_.memory() != nullptr
                && workspace_.config().capabilities.memory_bank;
    menu_->set_memory_bank_item_enabled(can_use);
    if (!can_use && memory_bank_panel_) {
        if (auto& p = aui_.GetPane("memory_bank"); p.IsOk() && p.IsShown()) {
            p.Show(false);
            aui_.Update();
            menu_->set_memory_bank_pane_visible(false);
        }
    }
    if (memory_bank_panel_) {
        memory_bank_panel_->set_store(can_use ? workspace_.memory() : nullptr);
    }
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
