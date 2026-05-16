#include "menu_controller.h"
#include "recent_workspaces.h"

#include "../../core/global_config.h"
#include "../../core/global_paths.h"

#include <wx/aboutdlg.h>
#include <wx/dirdlg.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace locus {

namespace {
    enum {
        ID_MENU_QUIT             = wxID_EXIT,
        ID_MENU_ABOUT            = wxID_ABOUT,
        ID_MENU_COMPACT          = wxID_HIGHEST + 200,
        ID_MENU_OPEN_WORKSPACE,
        ID_MENU_OPEN_WORKSPACE_FOLDER,
        ID_MENU_SETTINGS,
        ID_MENU_OPEN_GLOBAL_CONFIG,
        ID_MENU_OPEN_GLOBAL_FOLDER,
        ID_MENU_VIEW_FILES,
        ID_MENU_VIEW_ACTIVITY,
        ID_MENU_VIEW_TERMINAL,
        ID_MENU_NEW_TAB,
        ID_MENU_CLOSE_TAB,
        ID_MENU_RENAME_TAB,
        ID_MENU_MANAGE_SESSIONS,
        ID_MENU_CLEANUP_SESSIONS,
        ID_MENU_RECENT_BASE         = wxID_HIGHEST + 300,  // 300..309
        ID_MENU_SESSION_OPEN_BASE   = wxID_HIGHEST + 400,  // 400..449
        ID_MENU_SESSION_DELETE_BASE = wxID_HIGHEST + 500,  // 500..549
        ID_MENU_SESSION_RENAME_BASE = wxID_HIGHEST + 600,  // 600..649
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

    file_menu->AppendSeparator();
    file_menu->Append(ID_MENU_OPEN_WORKSPACE_FOLDER, "Open Workspace Folder");
    file_menu->Append(ID_MENU_OPEN_GLOBAL_FOLDER,    "Open Global Locus Folder");
    file_menu->AppendSeparator();
    file_menu->Append(ID_MENU_SETTINGS,       "Settings...\tCtrl+,");
    file_menu->Append(ID_MENU_OPEN_GLOBAL_CONFIG, "Open Global Config...");
    file_menu->AppendSeparator();
    file_menu->Append(ID_MENU_QUIT, "Quit\tCtrl+Q");

    // -- Session menu (S5.I redesign) ----------------------------------------
    auto* session_menu = new wxMenu;
    // S5.I -- per-letter mnemonics so the UIA driver (and keyboard users) can
    // navigate to any entry without arrow-key bouncing. Letters chosen to be
    // unique within the Session menu so a single Alt-shortcut activates the
    // intended item: N(ew) / C(lose) / R(ename) / S(aved) / Co(m)pact /
    // (U)p Old / Mana(g)e. The Manage mnemonic deliberately uses `&g` because
    // `m` already belongs to Co&mpact.
    session_menu->Append(ID_MENU_NEW_TAB,    "&New Tab\tCtrl+T");
    session_menu->Append(ID_MENU_CLOSE_TAB,  "&Close Tab\tCtrl+W");
    session_menu->Append(ID_MENU_RENAME_TAB, "&Rename Active Tab...");
    session_menu->AppendSeparator();
    sessions_menu_ = new wxMenu;
    rebuild_sessions_menu(hooks_.provide_sessions ? hooks_.provide_sessions()
                                                  : std::vector<SessionInfo>{});
    session_menu->AppendSubMenu(sessions_menu_, "&Saved Sessions");
    session_menu->AppendSeparator();
    session_menu->Append(ID_MENU_COMPACT,           "Co&mpact Context");
    session_menu->AppendSeparator();
    session_menu->Append(ID_MENU_CLEANUP_SESSIONS,  "Clean &Up Old Sessions...");
    session_menu->Append(ID_MENU_MANAGE_SESSIONS,   "Mana&ge Sessions...");

    session_menu->Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& e) {
        if (hooks_.provide_sessions)
            rebuild_sessions_menu(hooks_.provide_sessions());
        e.Skip();
    });

    auto* view_menu = new wxMenu;
    view_files_item_    = view_menu->AppendCheckItem(ID_MENU_VIEW_FILES,    "Files Panel");
    view_activity_item_ = view_menu->AppendCheckItem(ID_MENU_VIEW_ACTIVITY, "Activity Panel");
    view_terminal_item_ = view_menu->AppendCheckItem(ID_MENU_VIEW_TERMINAL, "Terminal\tCtrl+`");
    view_files_item_->Check(true);
    view_activity_item_->Check(true);
    view_terminal_item_->Check(false);

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
        if (hooks_.on_compact) hooks_.on_compact();
    }, ID_MENU_COMPACT);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_new_tab) hooks_.on_new_tab();
    }, ID_MENU_NEW_TAB);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_close_active_tab) hooks_.on_close_active_tab();
    }, ID_MENU_CLOSE_TAB);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_rename_active_tab) hooks_.on_rename_active_tab();
    }, ID_MENU_RENAME_TAB);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_manage_sessions) hooks_.on_manage_sessions();
    }, ID_MENU_MANAGE_SESSIONS);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_cleanup_sessions) hooks_.on_cleanup_sessions();
    }, ID_MENU_CLEANUP_SESSIONS);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_settings) hooks_.on_settings();
    }, ID_MENU_SETTINGS);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (hooks_.on_open_workspace_folder) hooks_.on_open_workspace_folder();
    }, ID_MENU_OPEN_WORKSPACE_FOLDER);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        auto dir = global_paths::global_dir();
        if (dir.empty()) {
            wxMessageBox("Could not resolve the global Locus folder "
                         "(no HOME / USERPROFILE).",
                         "Locus", wxOK | wxICON_ERROR, frame_);
            return;
        }
        wxString url = wxString::FromUTF8(dir.string());
        if (!wxLaunchDefaultApplication(url)) {
            wxMessageBox(wxString::Format(
                "Could not open %s. Open it manually.", url),
                "Locus", wxOK | wxICON_WARNING, frame_);
        }
    }, ID_MENU_OPEN_GLOBAL_FOLDER);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        auto path = global_paths::config_path();
        if (path.empty()) {
            wxMessageBox("Could not resolve the global config path "
                         "(no HOME / USERPROFILE).",
                         "Locus", wxOK | wxICON_ERROR, frame_);
            return;
        }

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            if (!save_global_config(GlobalConfig{})) {
                wxMessageBox("Failed to create the global config file. "
                             "Check the log for details.",
                             "Locus", wxOK | wxICON_ERROR, frame_);
                return;
            }
        }

        wxString url = wxString::FromUTF8(path.string());
        if (!wxLaunchDefaultApplication(url)) {
            wxMessageBox(wxString::Format(
                "Could not open %s in your default editor. "
                "Open it manually to edit.", url),
                "Locus", wxOK | wxICON_WARNING, frame_);
        }
    }, ID_MENU_OPEN_GLOBAL_CONFIG);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        if (hooks_.on_toggle_files_pane) hooks_.on_toggle_files_pane(e.IsChecked());
    }, ID_MENU_VIEW_FILES);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        if (hooks_.on_toggle_activity_pane) hooks_.on_toggle_activity_pane(e.IsChecked());
    }, ID_MENU_VIEW_ACTIVITY);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        if (hooks_.on_toggle_terminal_pane) hooks_.on_toggle_terminal_pane(e.IsChecked());
    }, ID_MENU_VIEW_TERMINAL);

    // Saved Sessions per-entry handlers.
    frame_->Bind(wxEVT_MENU, &MenuController::on_session_open,   this,
                 ID_MENU_SESSION_OPEN_BASE,
                 ID_MENU_SESSION_OPEN_BASE + static_cast<int>(k_max_sessions_in_menu) - 1);
    frame_->Bind(wxEVT_MENU, &MenuController::on_session_delete, this,
                 ID_MENU_SESSION_DELETE_BASE,
                 ID_MENU_SESSION_DELETE_BASE + static_cast<int>(k_max_sessions_in_menu) - 1);
    frame_->Bind(wxEVT_MENU, &MenuController::on_session_rename, this,
                 ID_MENU_SESSION_RENAME_BASE,
                 ID_MENU_SESSION_RENAME_BASE + static_cast<int>(k_max_sessions_in_menu) - 1);

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

    // S5.I -- filter out currently-open tabs so each session appears in at
    // most one place (in the tab strip OR in the Saved Sessions submenu).
    std::unordered_set<std::string> open_ids;
    if (hooks_.provide_open_ids) open_ids = hooks_.provide_open_ids();

    std::vector<const SessionInfo*> closed;
    closed.reserve(sessions.size());
    for (const auto& s : sessions) {
        if (open_ids.count(s.id) == 0) closed.push_back(&s);
    }

    if (closed.empty()) {
        sessions_menu_->Append(wxID_ANY, "(none)")->Enable(false);
        return;
    }

    const size_t n = std::min(closed.size(), k_max_sessions_in_menu);
    sessions_in_menu_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& s = *closed[i];

        std::string display = !s.title.empty() ? s.title : s.first_user_message;
        if (display.size() > 48) display = display.substr(0, 45) + "...";
        if (display.empty()) display = s.id;
        wxString label = wxString::Format("%s  (%d msgs)",
            wxString::FromUTF8(display), s.message_count);

        auto* entry_menu = new wxMenu;
        entry_menu->Append(ID_MENU_SESSION_OPEN_BASE   + static_cast<int>(i), "Open");
        entry_menu->Append(ID_MENU_SESSION_RENAME_BASE + static_cast<int>(i), "Rename...");
        entry_menu->Append(ID_MENU_SESSION_DELETE_BASE + static_cast<int>(i), "Delete...");

        sessions_menu_->AppendSubMenu(entry_menu, label);
        sessions_in_menu_.push_back(s.id);
    }

    if (closed.size() > k_max_sessions_in_menu) {
        sessions_menu_->AppendSeparator();
        sessions_menu_->Append(wxID_ANY,
            wxString::Format("(%zu more not shown - open Manage Sessions...)",
                             closed.size() - k_max_sessions_in_menu))
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

void MenuController::set_terminal_pane_visible(bool visible)
{
    if (view_terminal_item_) view_terminal_item_->Check(visible);
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

void MenuController::on_session_rename(wxCommandEvent& evt)
{
    size_t i = static_cast<size_t>(evt.GetId() - ID_MENU_SESSION_RENAME_BASE);
    if (i >= sessions_in_menu_.size()) return;
    if (hooks_.on_rename_session) hooks_.on_rename_session(sessions_in_menu_[i]);
}

} // namespace locus
