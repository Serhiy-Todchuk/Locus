#pragma once

#include "ansi_parser.h"
#include "../../tools/process_sink.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace locus {

// S5.R -- per-tab terminal model. Owns the canonical view that the
// TerminalPanel widget renders. Lives off `LocusTab` (no wx dependency,
// linked into `locus_core` alongside `AnsiParser`).
//
// Workers (sync `RunCommandTool`, bg-process reader threads) write into
// this object through the `IProcessSink` interface -- they enqueue raw
// bytes + lifecycle events under `mu_` and return quickly. A separate UI
// thread (`TerminalPanel`'s flush timer when this state is active) drains
// the queues, runs the per-sub-tab ANSI parser, appends parsed events to
// the canonical event log, and writes styled bytes into its wx STC pages.
//
// When the widget is NOT bound to this state (the tab is in the
// background), pending bytes accumulate -- on activation the widget
// drains the backlog before resuming live rendering. The backlog is
// bounded in practice by `BackgroundProcess`'s ring-buffer cap.

// One sub-tab inside the terminal panel state. Either the reserved sync
// "Run" tab (id == k_sync_id) or a bg-process tab (positive id assigned
// by `ProcessRegistry::spawn`).
struct TerminalTabState {
    int                    id              = 0;
    std::string            command;
    bool                   active          = true;
    int                    exit_code       = 0;
    bool                   killed          = false;
    bool                   timed_out       = false;
    bool                   stick_to_bottom = true;
    AnsiParser             parser;   // mutated under parent's mu_
    std::vector<AnsiEvent> events;   // canonical render log
    int                    line_count = 0; // running newline count in `events`
};

class TerminalPanelState : public IProcessSink {
public:
    static constexpr int k_sync_id = 0;

    explicit TerminalPanelState(std::size_t max_lines_per_tab = 10000);
    ~TerminalPanelState() override;

    TerminalPanelState(const TerminalPanelState&)            = delete;
    TerminalPanelState& operator=(const TerminalPanelState&) = delete;

    // -- IProcessSink (worker threads) --
    void on_bg_started(int id, const std::string& command) override;
    void on_bg_chunk  (int id, const char* data, std::size_t n) override;
    void on_bg_exited (int id, int exit_code, bool killed) override;
    void on_sync_started(const std::string& command) override;
    void on_sync_chunk  (const char* data, std::size_t n) override;
    void on_sync_exited (int exit_code, bool timed_out) override;

    enum class LifeKind { sync_start, sync_exit, bg_start, bg_exit };

    struct LifeEvent {
        LifeKind     kind      = LifeKind::sync_start;
        int          id        = 0;
        std::string  command;
        int          exit_code = 0;
        bool         killed    = false;
        bool         timed_out = false;
    };

    // -- UI-thread API. Each call locks internally. --

    // Swap-out the pending queues onto the caller. The caller (the widget's
    // flush timer) walks `lifecycle` first to create/teardown sub-tabs, then
    // iterates `text` to feed the per-sub-tab parser. Returns true iff
    // anything was drained. drain_pending also applies lifecycle effects to
    // the canonical tab map so subsequent reads see the post-event state.
    bool drain_pending(std::deque<LifeEvent>& lifecycle,
                       std::unordered_map<int, std::string>& text);

    // Walks `chunk` through the sub-tab's ANSI parser and appends the parsed
    // events to its canonical log (trimming to `max_lines_per_tab_`). Returns
    // the newly produced events so the widget can write them straight to its
    // STC without re-parsing. Empty result when `id` is unknown.
    std::vector<AnsiEvent> parse_and_append(int id, const std::string& chunk);

    // Clear a sub-tab's event log + ANSI parser (used by a fresh `sync_start`
    // before any chunks land). No-op if `id` is unknown.
    void clear_log(int id);

    // Snapshot of the sub-tab ordering (stable across calls, ordered by
    // creation -- Run tab first, bg tabs in spawn order). Cheap copy.
    std::vector<int> tab_ids_in_order() const;

    // Lightweight read-only snapshot for the widget to compose tab titles
    // (badge state, command). Returns std::nullopt if `id` is unknown.
    struct TabSnapshot {
        int          id              = 0;
        std::string  command;
        bool         active          = true;
        int          exit_code       = 0;
        bool         killed          = false;
        bool         timed_out       = false;
        bool         stick_to_bottom = true;
    };
    std::optional<TabSnapshot> snapshot(int id) const;

    // Clone of the parsed event log for replay during state activation.
    // Returns empty vector for unknown ids.
    std::vector<AnsiEvent> events_clone(int id) const;

    // Active sub-tab id (which tab the widget should focus on activation).
    int  active_sub_tab_id() const;
    void set_active_sub_tab_id(int id);

    // Mutate the stick-to-bottom flag from the widget when the user scrolls.
    void set_stick_to_bottom(int id, bool stick);

    std::size_t max_lines_per_tab() const { return max_lines_per_tab_; }

private:
    // Internal helpers (mu_ held).
    TerminalTabState* ensure_tab_locked_(int id, const std::string& command);
    void              trim_locked_(TerminalTabState& tab);

    mutable std::mutex                                       mu_;
    std::deque<LifeEvent>                                    pending_lifecycle_;
    std::unordered_map<int, std::string>                     pending_text_;
    std::unordered_map<int, std::unique_ptr<TerminalTabState>> tabs_;
    std::vector<int>                                         tab_order_;
    int                                                      active_sub_tab_id_ = k_sync_id;
    std::size_t                                              max_lines_per_tab_ = 10000;
};

// S5.R -- pure auto-show predicate. The terminal pane auto-shows on the
// first process spawned by the active tab, unless the user has manually
// hidden the pane since the last auto-show. Pulled out as a free function
// (rather than living on LocusFrame) so locus_core / locus_tests can verify
// it without pulling in wx.
inline bool should_auto_show_terminal(int event_tab_id, int active_tab_id,
                                       bool pane_hidden,
                                       bool user_hid_since_last_show)
{
    if (event_tab_id != active_tab_id) return false;
    if (!pane_hidden) return false;
    if (user_hid_since_last_show) return false;
    return true;
}

} // namespace locus
