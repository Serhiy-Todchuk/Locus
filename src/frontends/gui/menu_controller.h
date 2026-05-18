#pragma once

#include "../../agent/session_manager.h"

#include <wx/wx.h>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace locus {

// Owns the application menu bar and its dynamic submenus (Recent Workspaces,
// Saved Sessions). Holds no app state itself -- every action runs through the
// hooks supplied at construction. LocusFrame supplies the hooks.
class MenuController {
public:
    struct Hooks {
        std::function<void()>                       on_quit;
        std::function<void()>                       on_open_workspace_dialog;
        std::function<void(std::string)>            on_open_recent;
        // S5.M follow-up -- open the current workspace root in the OS file
        // manager. LocusFrame supplies the path; MenuController never holds
        // workspace state itself.
        std::function<void()>                       on_open_workspace_folder;
        std::function<void()>                       on_settings;
        std::function<void()>                       on_compact;
        // S5.I -- replaces Reset Conversation (no longer exists -- open a new
        // tab instead) and Save Session (continuous auto-save).
        std::function<void()>                       on_new_tab;          // Ctrl+T
        std::function<void()>                       on_close_active_tab; // Ctrl+W
        std::function<void()>                       on_rename_active_tab;
        std::function<void(std::string)>            on_load_session;     // Open Saved Sessions ▶ X ▶ Open
        std::function<void(std::string)>            on_rename_session;   // Open Saved Sessions ▶ X ▶ Rename
        std::function<void(std::string)>            on_delete_session;   // Open Saved Sessions ▶ X ▶ Delete
        std::function<void()>                       on_manage_sessions;  // Session ▶ Manage Sessions...
        std::function<void()>                       on_cleanup_sessions; // Session ▶ Clean Up Old Sessions...
        std::function<void(bool show)>              on_toggle_files_pane;
        std::function<void(bool show)>              on_toggle_activity_pane;
        // S5.B -- Terminal panel toggle (Ctrl+`).
        std::function<void(bool show)>              on_toggle_terminal_pane;
        // S5.K -- Memory Bank panel toggle (Ctrl+M).
        std::function<void(bool show)>              on_toggle_memory_bank_pane;
        // S5.Z task 2 -- chat find bar toggle (Ctrl+F). LocusFrame routes to
        // the active tab's ChatPanel.
        std::function<void()>                       on_toggle_find_in_chat;
        std::function<void()>                       on_about;
        // Called when the Session menu opens -- controller uses the returned
        // list to rebuild the Saved Sessions submenu so newly-saved sessions
        // appear without restarting the app.
        std::function<std::vector<SessionInfo>()>   provide_sessions;
        // S5.I -- ids currently open in tabs. The controller filters these
        // out of the Saved Sessions submenu so each session appears in at
        // most one place at any time.
        std::function<std::unordered_set<std::string>()> provide_open_ids;
    };

    MenuController(wxFrame* frame, Hooks hooks);

    // Build the wxMenuBar and attach it to the owning frame. Idempotent only
    // in the sense that calling twice is undefined -- call exactly once.
    void install();

    // Rebuild the "Recent Workspaces" submenu from disk.
    void rebuild_recent_menu();

    // Rebuild the "Saved Sessions" submenu from a fresh session list.
    void rebuild_sessions_menu(const std::vector<SessionInfo>& sessions);

    // Sync the View-menu checkboxes with current AUI pane visibility.
    void set_files_pane_visible(bool visible);
    void set_activity_pane_visible(bool visible);
    void set_terminal_pane_visible(bool visible);
    void set_memory_bank_pane_visible(bool visible);
    void set_memory_bank_item_enabled(bool enabled);

private:
    void on_session_open(wxCommandEvent& evt);
    void on_session_delete(wxCommandEvent& evt);
    void on_session_rename(wxCommandEvent& evt);

    wxFrame*  frame_;
    Hooks     hooks_;
    wxMenu*   recent_menu_   = nullptr;
    wxMenu*   sessions_menu_ = nullptr;
    wxMenuItem* view_files_item_       = nullptr;
    wxMenuItem* view_activity_item_    = nullptr;
    wxMenuItem* view_terminal_item_    = nullptr;
    wxMenuItem* view_memory_bank_item_ = nullptr;

    // Session IDs currently shown in the Saved Sessions submenu, indexed by
    // the offset from ID_MENU_SESSION_OPEN_BASE / _DELETE_BASE / _RENAME_BASE.
    std::vector<std::string> sessions_in_menu_;
};

} // namespace locus
