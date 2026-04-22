#pragma once

#include "activity_panel.h"
#include "chat_panel.h"
#include "compaction_dialog.h"
#include "file_tree_panel.h"
#include "locus_tray.h"
#include "settings_dialog.h"
#include "ask_user_dialog.h"
#include "tool_approval_panel.h"
#include "wx_frontend.h"
#include "../agent_core.h"
#include "../file_watcher.h"
#include "../workspace.h"

#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/timer.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace locus {

// Main application window. Owns the AUI layout with three panes:
//   left:   file tree with index status
//   center: chat panel (wxWebView + input + footer)
//   right:  detail panel (placeholder)
//
// Also owns the system tray icon and the WxFrontend thread bridge.
class LocusFrame : public wxFrame {
public:
    LocusFrame(AgentCore& agent, Workspace& workspace);
    ~LocusFrame() override;

    LocusFrame(const LocusFrame&) = delete;
    LocusFrame& operator=(const LocusFrame&) = delete;

private:
    void create_menu_bar();
    void create_status_bar();
    void setup_aui_layout();

    // Toggle visibility of a managed AUI pane by name, keeping its
    // matching View-menu checkbox in sync.
    void toggle_pane(const wxString& pane_name, bool show);

    // Refresh View-menu checkboxes from the current AUI pane visibility.
    void sync_view_menu();

    // Show the compaction dialog and apply the chosen strategy.
    void show_compaction_dialog();

    // Show the settings dialog and apply changes.
    void show_settings_dialog();

    // Rebuild the "Recent Workspaces" submenu from disk.
    void rebuild_recent_menu();

    // Rebuild the "Saved Sessions" submenu from disk.
    void rebuild_sessions_menu();

    // Handlers for the "Saved Sessions" submenu — bound once over an ID
    // range; the index into sessions_in_menu_ identifies the target session.
    void on_session_open(wxCommandEvent& evt);
    void on_session_delete(wxCommandEvent& evt);

    // Update file tree index stats from current workspace.
    void refresh_index_stats();

    // File-watcher pump. Drains the watcher queue on a timer, accumulates
    // events into pending_events_, and flushes (calls Indexer::process_events
    // once for the whole group) after a quiet period or a hard cap.
    void on_watcher_timer(wxTimerEvent& evt);
    void flush_pending_events();

    // Event handlers: window lifecycle
    void on_close(wxCloseEvent& evt);
    void on_iconize(wxIconizeEvent& evt);
    void on_aui_pane_close(wxAuiManagerEvent& evt);

    // Event handlers: agent thread events (via WxFrontend)
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

    // Recompose pane 1 (right, ops status) from current progress state.
    void refresh_ops_status();

    // Core references (not owned)
    AgentCore& agent_;
    Workspace& workspace_;

    // Owned
    wxAuiManager           aui_;
    std::unique_ptr<LocusTray>    tray_;
    std::unique_ptr<WxFrontend>   wx_frontend_;

    // Pane panels.
    FileTreePanel*     file_tree_panel_ = nullptr;  // file tree + index status (S1.6)
    ChatPanel*         chat_panel_     = nullptr;   // chat UI (S1.3)
    ToolApprovalPanel* approval_panel_ = nullptr;   // tool approval (S1.4)
    ActivityPanel*     activity_panel_ = nullptr;   // activity log (S2.2)
    wxMenu*            recent_menu_    = nullptr;   // "Recent Workspaces" submenu (owned by menu bar)
    wxMenu*            sessions_menu_  = nullptr;   // "Saved Sessions" submenu (owned by menu bar)
    wxMenuItem*        view_files_item_    = nullptr;
    wxMenuItem*        view_activity_item_ = nullptr;

    // Session IDs currently shown in the Saved Sessions submenu, indexed
    // by the offset from ID_MENU_SESSION_OPEN_BASE / _DELETE_BASE.
    std::vector<std::string> sessions_in_menu_;

    // Background-op progress state. `total == 0` means the op is not running;
    // `done == total` (both > 0) means it just completed and should be hidden.
    int indexing_done_   = 0;
    int indexing_total_  = 0;
    int embedding_done_  = 0;
    int embedding_total_ = 0;

    // File-watcher batching state (see on_watcher_timer / flush_pending_events).
    wxTimer                               watcher_timer_;
    std::vector<FileEvent>                pending_events_;
    std::chrono::steady_clock::time_point group_start_{};
    std::chrono::steady_clock::time_point last_event_time_{};
    bool                                  group_active_ = false;

    wxDECLARE_EVENT_TABLE();
};

} // namespace locus
