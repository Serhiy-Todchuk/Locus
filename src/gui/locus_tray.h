#pragma once

#include <wx/taskbar.h>
#include <wx/wx.h>

namespace locus {

class LocusFrame;

// System tray icon with state display and right-click menu.
// States: idle, active (agent working), error.
class LocusTray : public wxTaskBarIcon {
public:
    explicit LocusTray(LocusFrame* frame);

    enum class State { idle, active, error };
    void set_state(State s);

protected:
    wxMenu* CreatePopupMenu() override;

private:
    void on_left_click(wxTaskBarIconEvent& evt);
    void on_show(wxCommandEvent& evt);
    void on_quit(wxCommandEvent& evt);

    LocusFrame* frame_;
    State       state_ = State::idle;

    wxDECLARE_EVENT_TABLE();
};

} // namespace locus
