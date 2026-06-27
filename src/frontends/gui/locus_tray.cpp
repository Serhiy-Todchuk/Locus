#include "locus_tray.h"
#include "app_icons.h"
#include "locus_frame.h"

namespace locus {

enum {
    ID_TRAY_SHOW = wxID_HIGHEST + 100,
    ID_TRAY_QUIT
};

wxBEGIN_EVENT_TABLE(LocusTray, wxTaskBarIcon)
    EVT_TASKBAR_LEFT_DCLICK(LocusTray::on_left_click)
wxEND_EVENT_TABLE()

LocusTray::LocusTray(LocusFrame* frame)
    : frame_(frame)
{
    set_state(State::idle);
}

void LocusTray::set_state(State s)
{
    state_ = s;

    wxIcon icon;
    wxString tooltip;

    switch (s) {
    case State::idle:
        icon = gui::tray_idle_icon();
        tooltip = "Locus - idle";
        break;
    case State::indexing:
        icon = gui::tray_indexing_icon();
        tooltip = "Locus - indexing...";
        break;
    case State::active:
        icon = gui::tray_active_icon();
        tooltip = "Locus - working...";
        break;
    case State::error:
        icon = gui::tray_error_icon();
        tooltip = "Locus - error";
        break;
    }

    SetIcon(icon, tooltip);
}

wxMenu* LocusTray::CreatePopupMenu()
{
    auto* menu = new wxMenu;
    menu->Append(ID_TRAY_SHOW, frame_->IsShown() ? "Hide Window" : "Show Window");
    menu->AppendSeparator();
    menu->Append(ID_TRAY_QUIT, "Quit Locus");

    menu->Bind(wxEVT_MENU, &LocusTray::on_show, this, ID_TRAY_SHOW);
    menu->Bind(wxEVT_MENU, &LocusTray::on_quit, this, ID_TRAY_QUIT);
    return menu;
}

void LocusTray::on_left_click(wxTaskBarIconEvent& /*evt*/)
{
    restore_frame();
}

void LocusTray::on_show(wxCommandEvent& /*evt*/)
{
    if (frame_->IsShown())
        frame_->Hide();
    else
        restore_frame();
}

void LocusTray::restore_frame()
{
    if (!frame_->IsShown())
        frame_->Show(true);
    // De-iconize so a window that was minimized before being hidden comes
    // back as a normal restored window, not an invisible minimized one.
    if (frame_->IsIconized())
        frame_->Iconize(false);
    frame_->Raise();
}

void LocusTray::on_quit(wxCommandEvent& /*evt*/)
{
    frame_->Close(true);
}

} // namespace locus
