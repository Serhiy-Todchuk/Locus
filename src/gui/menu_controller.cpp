#include "menu_controller.h"
#include "recent_workspaces.h"

#include <wx/aboutdlg.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <utility>

namespace locus {

namespace {
    enum {
        ID_MENU_QUIT             = wxID_EXIT,
        ID_MENU_ABOUT            = wxID_ABOUT,
        ID_MENU_RESET            = wxID_HIGHEST + 200,
        ID_MENU_COMPACT,
        ID_MENU_SAVE_SESSION,
        ID_MENU_OPEN_WORKSPACE,
        ID_MENU_SETTINGS,
        ID_MENU_VIEW_FILES,
        ID_MENU_VIEW_ACTIVITY,
        ID_MENU_CLEAR_SESSIONS,
        ID_MENU_RECENT_BASE         = wxID_HIGHEST + 300,  // 300..309
        ID_MENU_SESSION_OPEN_BASE   = wxID_HIGHEST + 400,  // 400..449
        ID_MENU_SESSION_DELETE_BASE = wxID_HIGHEST + 500,  // 500..549
    };

    constexpr size_t k_max_sessions_in_menu = 50;
}

MenuController::MenuController(wxFrame* frame, Hooks hooks)
    : frame_(frame), hooks_(std::move(hooks))
{}

void MenuController::install()
{
    auto* file_menu = new wxMenu;
    file_menu->Append(ID_MENU_OPEN_WORKSPACE, "Open Workspace...\tCtrl+O");

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
    rebuild_sessions_menu(hooks_.provide_sessions ? hooks_.provide_sessions()
                                                  : std::vector<SessionInfo>{});
    session_menu->AppendSubMenu(sessions_menu_, "Saved Sessions");
    session_menu->AppendSeparator();
    session_menu->Append(ID_MENU_CLEAR_SESSIONS, "Clear All Sessions...");

    // Refresh the sessions submenu each time the Session menu opens so new
    // saves and external removals show up without restarting the app.
    session_menu->Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& e) {
        if (hooks_.provide_sessions)
            rebuild_sessions_menu(hooks_.provide_sessions());
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
    frame_->SetMenuBar(menu_bar);

    // Top-level menu handlers.
    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_quit) hooks_.on_quit();
    }, ID_MENU_QUIT);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_open_workspace_dialog) hooks_.on_open_workspace_dialog();
    }, ID_MENU_OPEN_WORKSPACE);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_reset_conversation) hooks_.on_reset_conversation();
    }, ID_MENU_RESET);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_compact) hooks_.on_compact();
    }, ID_MENU_COMPACT);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_save_session) hooks_.on_save_session();
    }, ID_MENU_SAVE_SESSION);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_clear_sessions) hooks_.on_clear_sessions();
    }, ID_MENU_CLEAR_SESSIONS);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_settings) hooks_.on_settings();
    }, ID_MENU_SETTINGS);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        if (hooks_.on_toggle_files_pane) hooks_.on_toggle_files_pane(e.IsChecked());
    }, ID_MENU_VIEW_FILES);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        if (hooks_.on_toggle_activity_pane) hooks_.on_toggle_activity_pane(e.IsChecked());
    }, ID_MENU_VIEW_ACTIVITY);

    // Saved Sessions submenu: bind the whole ID range once.
    frame_->Bind(wxEVT_MENU, &MenuController::on_session_open,   this,
                 ID_MENU_SESSION_OPEN_BASE,
                 ID_MENU_SESSION_OPEN_BASE + static_cast<int>(k_max_sessions_in_menu) - 1);
    frame_->Bind(wxEVT_MENU, &MenuController::on_session_delete, this,
                 ID_MENU_SESSION_DELETE_BASE,
                 ID_MENU_SESSION_DELETE_BASE + static_cast<int>(k_max_sessions_in_menu) - 1);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_about) hooks_.on_about();
    }, ID_MENU_ABOUT);
}

void MenuController::rebuild_recent_menu()
{
    if (!recent_menu_) return;

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
        frame_->Bind(wxEVT_MENU, [this, path = entries[i]](wxCommandEvent&) {
            if (hooks_.on_open_recent) hooks_.on_open_recent(path);
        }, id);
    }
}

void MenuController::rebuild_sessions_menu(const std::vector<SessionInfo>& sessions)
{
    if (!sessions_menu_) return;

    while (sessions_menu_->GetMenuItemCount() > 0)
        sessions_menu_->Delete(sessions_menu_->FindItemByPosition(0));

    sessions_in_menu_.clear();

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

void MenuController::set_files_pane_visible(bool visible)
{
    if (view_files_item_) view_files_item_->Check(visible);
}

void MenuController::set_activity_pane_visible(bool visible)
{
    if (view_activity_item_) view_activity_item_->Check(visible);
}

void MenuController::on_session_open(wxCommandEvent& evt)
{
    size_t i = static_cast<size_t>(evt.GetId() - ID_MENU_SESSION_OPEN_BASE);
    if (i >= sessions_in_menu_.size()) return;
    if (hooks_.on_load_session) hooks_.on_load_session(sessions_in_menu_[i]);
}

void MenuController::on_session_delete(wxCommandEvent& evt)
{
    size_t i = static_cast<size_t>(evt.GetId() - ID_MENU_SESSION_DELETE_BASE);
    if (i >= sessions_in_menu_.size()) return;
    if (hooks_.on_delete_session) hooks_.on_delete_session(sessions_in_menu_[i]);
}

} // namespace locus
