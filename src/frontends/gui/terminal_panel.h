#pragma once

#include "ansi_parser.h"
#include "terminal_panel_state.h"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/stc/stc.h>
#include <wx/timer.h>

#include <functional>
#include <string>
#include <unordered_map>

namespace locus {

// S5.B + S5.R -- live terminal panel widget. AUI-docked at the bottom of
// the main frame, default hidden; View menu "Terminal" (Ctrl+`) toggles
// visibility.
//
// S5.R turned the panel into a pure view over a per-tab `TerminalPanelState`
// (owned by `LocusTab`). Switching chat tabs swaps the state pointer via
// `set_state()`; the widget clears its inner `wxNotebook` of STC pages and
// rebuilds them from the new state's sub-tab list, replaying the canonical
// AnsiEvent log into the STC. The AUI pane's visibility stays workspace-
// wide -- only the contents swap.
//
// Output flow: worker threads push raw bytes + lifecycle events into the
// state's mutex-protected queues. The widget's ~30 fps `wxTimer` drains
// the **currently bound** state's queues, runs the per-sub-tab ANSI parser
// (via state->parse_and_append), appends events to the state's canonical
// log, and writes styled bytes to the STC pages. Inactive states accumulate
// backlog quietly; on activation the next timer tick processes it.
class TerminalPanel : public wxPanel {
public:
    using KillHandler = std::function<void(int bg_id)>;

    TerminalPanel(wxWindow* parent, KillHandler on_kill = {});
    ~TerminalPanel() override;

    TerminalPanel(const TerminalPanel&)            = delete;
    TerminalPanel& operator=(const TerminalPanel&) = delete;

    // S5.R: bind the widget to a `TerminalPanelState` (the active chat
    // tab's terminal). `nullptr` clears the notebook (empty view -- no
    // placeholder page). Idempotent if called with the same pointer.
    void set_state(TerminalPanelState* state);

    // S5.R observer hook -- LocusFrame attaches this to receive a
    // "first process spawned" signal from the currently active state on
    // each timer drain. Used for the auto-show heuristic. Empty by default.
    void set_first_command_observer(std::function<void()> obs);

private:
    enum {
        ID_TERM_COPY_ALL = wxID_HIGHEST + 1700,
        ID_TERM_COPY_SEL,
        ID_TERM_CLEAR,
        ID_TERM_KILL,
    };

    static constexpr int k_flush_interval_ms = 33;

    // Per-process-tab widget bookkeeping. Lives only while the parent state
    // is bound -- rebuilt from scratch on `set_state` from the state's
    // tab list. Never the source of truth; the state's TerminalTabState is.
    struct Page {
        int               id          = 0;
        wxPanel*          panel       = nullptr;
        wxStyledTextCtrl* stc         = nullptr;
        bool              cmd_header_written = false;
    };

    void on_flush_timer(wxTimerEvent& evt);
    void on_context_menu_action(wxCommandEvent& evt);

    void rebuild_from_state_();
    void clear_pages_();
    Page* ensure_page_(int id);
    int   find_notebook_index_(int id) const;
    void  write_command_header_(Page& page, const std::string& command);
    void  replay_events_(Page& page, const std::vector<AnsiEvent>& events);
    void  apply_ansi_event_(Page& page, const AnsiEvent& ev,
                            TerminalTabState* tab_state /*nullable*/);
    void  write_styled_(Page& page, const std::string& text, const AnsiStyle& style);
    void  trim_scrollback_(Page& page);
    void  set_tab_badge_(int idx, const TerminalPanelState::TabSnapshot& snap);
    void  ensure_style_table_(wxStyledTextCtrl* stc);
    static int style_id_for_(const AnsiStyle& s);

    wxNotebook*     notebook_ = nullptr;
    wxTimer         flush_timer_;
    KillHandler     kill_handler_;
    std::function<void()> first_command_observer_;

    TerminalPanelState* state_ = nullptr;
    std::unordered_map<int, Page> pages_;

    std::size_t max_lines_per_tab_ = 10000;

    wxDECLARE_EVENT_TABLE();
};

} // namespace locus
