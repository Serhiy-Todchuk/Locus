#include "locus_tray.h"
#include "locus_frame.h"

#include <wx/artprov.h>

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

    // Use a simple text icon via wxICON_INFORMATION / etc.
    // Real icons (ICO resource) will come in a later stage.
    wxIcon icon;
    wxString tooltip;

    switch (s) {
    case State::idle:
        icon = wxArtProvider::GetIcon(wxART_INFORMATION, wxART_OTHER, wxSize(16, 16));
        tooltip = "Locus - idle";
        break;
    case State::active:
        icon = wxArtProvider::GetIcon(wxART_EXECUTABLE_FILE, wxART_OTHER, wxSize(16, 16));
        tooltip = "Locus - working...";
        break;
    case State::error:
        icon = wxArtProvider::GetIcon(wxART_ERROR, wxART_OTHER, wxSize(16, 16));
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
    if (frame_->IsShown()) {
        frame_->Raise();
    } else {
        frame_->Show(true);
        frame_->Raise();
    }
}

void LocusTray::on_show(wxCommandEvent& /*evt*/)
{
    frame_->Show(!frame_->IsShown());
    if (frame_->IsShown())
        frame_->Raise();
}

void LocusTray::on_quit(wxCommandEvent& /*evt*/)
{
    frame_->Close(true);
}

} // namespace locus
