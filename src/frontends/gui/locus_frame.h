#pragma once

#include "activity_panel.h"
#include "chat_panel.h"
#include "compaction_dialog.h"
#include "file_tree_panel.h"
#include "locus_tray.h"
#include "menu_controller.h"
#include "ops_status_view.h"
#include "settings_dialog.h"
#include "ask_user_dialog.h"
#include "terminal_panel.h"
#include "tool_approval_dialog.h"
#include "ui_state.h"
#include "wx_frontend.h"
#include "../../core/locus_session.h"
#include "../../core/workspace.h"

#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/aui/auibook.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace locus {

class LocusTab;
class McpManager;

// Main application window. Owns the AUI layout with three workspace-shared
// panes (file tree / activity / terminal) plus a center `wxAuiNotebook`
// of per-tab `ChatPanel`s -- one tab per `LocusTab` in the `LocusSession`.
// Workspace-shared subsystems sit on the LocusSession; tab-scoped state
// (chat panel, per-tab WxFrontend) lives on a `TabUi` value in `tabs_ui_`.
class LocusFrame : public wxFrame {
public:
    explicit LocusFrame(LocusSession& session);
    ~LocusFrame() override;

    LocusFrame(const LocusFrame&) = delete;
    LocusFrame& operator=(const LocusFrame&) = delete;

    // Drop every reference into the session (frontend registration,
    // indexer / embedding-worker callbacks). Idempotent. Call this BEFORE
    // tearing down the LocusSession the frame was constructed against.
    void detach_from_session();

private:
    void create_status_bar();
    void setup_aui_layout();
    void apply_saved_window_geometry();

    // Tab-shell helpers.
    struct TabUi {
        int                          tab_id = 0;     // matches LocusTab::tab_id
        LocusTab*                    tab    = nullptr;
        ChatPanel*                   chat   = nullptr;
        std::unique_ptr<WxFrontend>  frontend;       // registered with the tab's agent
        // S5.I tab status badge (computed each event so we don't recompute
        // every render).
        bool busy             = false;
        bool awaiting_decision = false;
        bool ctx_over_warn   = false;
        bool ctx_over_auto   = false;
    };

    // Build a `ChatPanel` + `WxFrontend` for `tab` and add it to the notebook
    // at the end. Returns the matching TabUi entry.
    TabUi& install_tab_ui(LocusTab& tab, const wxString& tab_title);

    // Tear down the TabUi at index `nb_index` (notebook page index). Removes
    // the ChatPanel from the notebook, unregisters the WxFrontend, erases
    // the entry from tabs_ui_. The caller is responsible for the matching
    // LocusSession::close_tab / delete_tab.
    void teardown_tab_ui(int nb_index);

    // Repaint a tab's title with status indicators.
    void refresh_tab_title(int tab_id);
    wxString compose_tab_label(const TabUi& ui) const;

    // Lookup helpers.
    TabUi* find_tab_ui(int tab_id);
    int    notebook_index_for_tab_id(int tab_id) const;
    int    notebook_index_for_chat(const ChatPanel* chat) const;
    TabUi& active_tab_ui();
    LocusTab& active_tab();

    // Tab-shell action handlers.
    void on_new_tab();
    void on_close_active_tab();
    void on_rename_active_tab();
    void on_notebook_page_changed(wxAuiNotebookEvent& evt);
    void on_notebook_page_close(wxAuiNotebookEvent& evt);   // X button
    void on_notebook_tab_right_up(wxAuiNotebookEvent& evt); // context menu

    // Session-menu hooks.
    void on_open_saved_session(const std::string& session_id);
    void on_delete_saved_session(const std::string& session_id);
    void on_manage_sessions(bool prefilter_cleanup);

    // Compaction / settings dialogs.
    void show_compaction_dialog();
    void show_settings_dialog();
    void refresh_index_stats();

    // Window lifecycle.
    void on_close(wxCloseEvent& evt);
    void on_iconize(wxIconizeEvent& evt);
    void on_aui_pane_close(wxAuiManagerEvent& evt);

    // Agent thread events (via WxFrontend). All route by tab_id (evt.GetId()).
    void on_agent_turn_start(wxThreadEvent& evt);
    void on_agent_token(wxThreadEvent& evt);
    void on_agent_reasoning_token(wxThreadEvent& evt);
    void on_agent_tool_pending(wxThreadEvent& evt);
    void on_agent_tool_result(wxThreadEvent& evt);
    void on_agent_turn_complete(wxThreadEvent& evt);
    void on_agent_context_meter(wxThreadEvent& evt);
    void on_agent_compaction(wxThreadEvent& evt);
    void on_agent_session_reset(wxThreadEvent& evt);
    void on_agent_error(wxThreadEvent& evt);
    void on_agent_embedding_progress(wxThreadEvent& evt);
    void on_agent_indexing_progress(wxThreadEvent& evt);
    void on_agent_activity(wxThreadEvent& evt);
    void on_agent_attached_context(wxThreadEvent& evt);
    void on_agent_mode_changed(wxThreadEvent& evt);
    void on_agent_plan_proposed(wxThreadEvent& evt);
    void on_agent_plan_step_advanced(wxThreadEvent& evt);
    void on_agent_plan_completed(wxThreadEvent& evt);
    void on_agent_auto_commit(wxThreadEvent& evt);
    void on_agent_gen_progress(wxThreadEvent& evt);
    void on_agent_history_msg_added(wxThreadEvent& evt);
    void on_agent_history_msg_deleted(wxThreadEvent& evt);

    void refresh_ops_status();

    // Wire callbacks/slash items into a newly-constructed ChatPanel. Pulled
    // out so install_tab_ui can call it without massive duplication.
    void configure_chat_panel(ChatPanel* chat, LocusTab& tab);

    // Workspace-level resources, owned by LocusSession.
    LocusSession& session_;
    Workspace&    workspace_;
    McpManager*   mcp_ = nullptr;

    // Saved window + AUI layout from a previous session.
    UiState saved_ui_state_;

    // AUI manager + notebook (tabs of ChatPanels live inside the notebook).
    wxAuiManager      aui_;
    wxAuiNotebook*    notebook_ = nullptr;

    // System tray + menu.
    std::unique_ptr<LocusTray>      tray_;
    std::unique_ptr<MenuController> menu_;
    OpsStatusView                   ops_status_;

    // Workspace-shared panels.
    FileTreePanel* file_tree_panel_ = nullptr;
    ActivityPanel* activity_panel_  = nullptr;
    TerminalPanel* terminal_panel_  = nullptr;

    // Tab UI state, keyed by tab_id (== LocusTab::tab_id).
    std::unordered_map<int, TabUi> tabs_ui_;

    // Workspace-shared bridge for indexer/embedding-worker progress events
    // (not tied to any specific tab). Posts wxThreadEvents with tab_id=0.
    std::unique_ptr<WxFrontend> shared_bridge_;

    bool session_detached_ = false;

    wxDECLARE_EVENT_TABLE();
};

} // namespace locus
