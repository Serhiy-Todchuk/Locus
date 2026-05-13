#pragma once

#include "ansi_parser.h"
#include "../../tools/process_sink.h"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/stc/stc.h>
#include <wx/timer.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace locus {

class ProcessRegistry;

// S5.B -- live terminal panel. AUI-docked at the bottom of the main frame,
// default hidden; View menu "Terminal" (Ctrl+`) toggles visibility.
//
// One tab per running background process (sentinel id 0 reserved for the
// most recent synchronous `run_command`). Output is collected on worker
// threads via the IProcessSink interface, batched under a mutex, and flushed
// to wxStyledTextCtrl on a ~30 fps wxTimer to avoid pegging the UI thread
// on chatty output.
//
// Auto-scroll sticks to the bottom unless the user scrolls up; once they do,
// the panel stops auto-scrolling until the user manually scrolls back to the
// bottom (or clicks the "Scroll to bottom" affordance).
//
// Lifetime: the panel registers itself as the broker's sink in `attach`
// (called from LocusFrame after construction) and unregisters in `detach`
// before destruction. The broker must outlive any wired sink call.
class TerminalPanel : public wxPanel, public IProcessSink {
public:
    static constexpr int k_sync_id = 0;

    TerminalPanel(wxWindow* parent, ProcessSinkBroker* broker,
                  ProcessRegistry* registry, std::size_t max_lines_per_tab = 10000);
    ~TerminalPanel() override;

    TerminalPanel(const TerminalPanel&)            = delete;
    TerminalPanel& operator=(const TerminalPanel&) = delete;

    // Detach from the broker. Idempotent. Call before tearing down the
    // workspace / registry so no late chunk lands on a destroyed panel.
    void detach();

    // -- IProcessSink (worker threads) --
    void on_bg_started(int id, const std::string& command) override;
    void on_bg_chunk  (int id, const char* data, std::size_t n) override;
    void on_bg_exited (int id, int exit_code, bool killed) override;
    void on_sync_started(const std::string& command) override;
    void on_sync_chunk  (const char* data, std::size_t n) override;
    void on_sync_exited (int exit_code, bool timed_out) override;

private:
    enum class LifeKind { sync_start, sync_exit, bg_start, bg_exit };

    struct LifeEvent {
        LifeKind    kind;
        int         id;
        std::string command;
        int         exit_code = 0;
        bool        killed    = false;
        bool        timed_out = false;
    };

    struct Tab {
        int                 id;            // 0 = sync, positive = bg
        std::string         command;
        wxPanel*            page    = nullptr;
        wxStyledTextCtrl*   stc     = nullptr;
        AnsiParser          parser;        // UI-thread-only (read from flush_pending only)
        bool                active        = true;
        int                 exit_code     = 0;
        bool                killed        = false;
        bool                timed_out     = false;
        bool                stick_to_bottom = true;  // auto-scroll
    };

    // Worker-thread side: queue text + lifecycle events under a mutex. The
    // flush timer (UI thread) drains both into the tab structures.
    std::mutex                              mu_;
    std::deque<LifeEvent>                   pending_lifecycle_;
    std::unordered_map<int, std::string>    pending_text_;   // id -> appended text
    std::atomic<bool>                       attached_{true}; // false after detach()

    // UI-thread state.
    wxNotebook*                                       notebook_ = nullptr;
    wxTimer                                           flush_timer_;
    std::unordered_map<int, std::unique_ptr<Tab>>     tabs_;
    std::size_t                                       max_lines_per_tab_;
    ProcessSinkBroker*                                broker_   = nullptr; // unowned
    ProcessRegistry*                                  registry_ = nullptr; // unowned, may be null

    static constexpr int k_flush_interval_ms = 33;  // ~30 fps

    void   on_flush_timer(wxTimerEvent& evt);
    Tab*   ensure_tab(int id, const std::string& command);
    void   set_tab_badge(int idx, const Tab& tab);
    int    find_tab_index(int id) const;
    void   process_lifecycle(const LifeEvent& ev);
    void   append_to_tab(Tab& tab, const std::string& text);
    void   trim_scrollback(Tab& tab);
    void   apply_ansi_event(Tab& tab, const AnsiEvent& ev);
    void   write_styled(Tab& tab, const std::string& text, const AnsiStyle& style);
    void   ensure_style_table(wxStyledTextCtrl* stc);
    static int style_id_for(const AnsiStyle& s);
    void   on_tab_right_click(wxContextMenuEvent& evt);
    void   on_context_menu_action(wxCommandEvent& evt);

    wxDECLARE_EVENT_TABLE();
};

} // namespace locus
