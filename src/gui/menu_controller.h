#pragma once

#include "../session_manager.h"

#include <wx/wx.h>

#include <functional>
#include <string>
#include <vector>

namespace locus {

// Owns the application menu bar and its dynamic submenus (Recent Workspaces,
// Saved Sessions). Holds no app state itself — every action runs through the
// hooks supplied at construction. LocusFrame supplies the hooks.
class MenuController {
public:
    struct Hooks {
        std::function<void()>                       on_quit;
        std::function<void()>                       on_open_workspace_dialog;
        std::function<void(std::string)>            on_open_recent;
        std::function<void()>                       on_settings;
        std::function<void()>                       on_reset_conversation;
        std::function<void()>                       on_compact;
        std::function<void()>                       on_save_session;
        std::function<void()>                       on_clear_sessions;
        std::function<void(std::string)>            on_load_session;
        std::function<void(std::string)>            on_delete_session;
        std::function<void(bool show)>              on_toggle_files_pane;
        std::function<void(bool show)>              on_toggle_activity_pane;
        std::function<void()>                       on_about;
        // Called when the Session menu opens — controller uses the returned
        // list to rebuild the Saved Sessions submenu so newly-saved sessions
        // appear without restarting the app.
        std::function<std::vector<SessionInfo>()>   provide_sessions;
    };

    MenuController(wxFrame* frame, Hooks hooks);

    // Build the wxMenuBar and attach it to the owning frame. Idempotent only
    // in the sense that calling twice is undefined — call exactly once.
    void install();

    // Rebuild the "Recent Workspaces" submenu from disk.
    void rebuild_recent_menu();

    // Rebuild the "Saved Sessions" submenu from a fresh session list.
    void rebuild_sessions_menu(const std::vector<SessionInfo>& sessions);

    // Sync the View-menu checkboxes with current AUI pane visibility.
    void set_files_pane_visible(bool visible);
    void set_activity_pane_visible(bool visible);

private:
    void on_session_open(wxCommandEvent& evt);
    void on_session_delete(wxCommandEvent& evt);

    wxFrame*  frame_;
    Hooks     hooks_;
    wxMenu*   recent_menu_   = nullptr;
    wxMenu*   sessions_menu_ = nullptr;
    wxMenuItem* view_files_item_    = nullptr;
    wxMenuItem* view_activity_item_ = nullptr;

    // Session IDs currently shown in the Saved Sessions submenu, indexed by
    // the offset from ID_MENU_SESSION_OPEN_BASE / _DELETE_BASE.
    std::vector<std::string> sessions_in_menu_;
};

} // namespace locus
