#include "terminal_panel_state.h"

#include <algorithm>
#include <utility>

namespace locus {

TerminalPanelState::TerminalPanelState(std::size_t max_lines_per_tab)
    : max_lines_per_tab_(max_lines_per_tab)
{
    // Reserve the sync "Run" tab up front -- empty until the first sync
    // run_command lands. Matches S5.B's widget behaviour: the Run tab is
    // always present so its position in the notebook is stable.
    auto sync = std::make_unique<TerminalTabState>();
    sync->id              = k_sync_id;
    sync->command         = "(no command yet)";
    sync->active          = false;  // no sync run yet
    sync->stick_to_bottom = true;
    tabs_[k_sync_id]      = std::move(sync);
    tab_order_.push_back(k_sync_id);
}

TerminalPanelState::~TerminalPanelState() = default;

// -- IProcessSink (worker threads) -------------------------------------------

void TerminalPanelState::on_bg_started(int id, const std::string& command)
{
    std::lock_guard l(mu_);
    LifeEvent e;
    e.kind    = LifeKind::bg_start;
    e.id      = id;
    e.command = command;
    pending_lifecycle_.push_back(std::move(e));
}

void TerminalPanelState::on_bg_chunk(int id, const char* data, std::size_t n)
{
    if (n == 0) return;
    std::lock_guard l(mu_);
    pending_text_[id].append(data, n);
}

void TerminalPanelState::on_bg_exited(int id, int exit_code, bool killed)
{
    std::lock_guard l(mu_);
    LifeEvent e;
    e.kind      = LifeKind::bg_exit;
    e.id        = id;
    e.exit_code = exit_code;
    e.killed    = killed;
    pending_lifecycle_.push_back(std::move(e));
}

void TerminalPanelState::on_sync_started(const std::string& command)
{
    std::lock_guard l(mu_);
    LifeEvent e;
    e.kind    = LifeKind::sync_start;
    e.id      = k_sync_id;
    e.command = command;
    pending_lifecycle_.push_back(std::move(e));
    // Drop any leftover text that hadn't been flushed yet -- the lifecycle
    // event resets the sync tab.
    pending_text_.erase(k_sync_id);
}

void TerminalPanelState::on_sync_chunk(const char* data, std::size_t n)
{
    if (n == 0) return;
    std::lock_guard l(mu_);
    pending_text_[k_sync_id].append(data, n);
}

void TerminalPanelState::on_sync_exited(int exit_code, bool timed_out)
{
    std::lock_guard l(mu_);
    LifeEvent e;
    e.kind      = LifeKind::sync_exit;
    e.id        = k_sync_id;
    e.exit_code = exit_code;
    e.timed_out = timed_out;
    pending_lifecycle_.push_back(std::move(e));
}

// -- UI-thread API -----------------------------------------------------------

bool TerminalPanelState::drain_pending(std::deque<LifeEvent>& lifecycle,
                                       std::unordered_map<int, std::string>& text)
{
    std::lock_guard l(mu_);
    if (pending_lifecycle_.empty() && pending_text_.empty()) return false;
    lifecycle.clear();
    text.clear();
    lifecycle.swap(pending_lifecycle_);
    text.swap(pending_text_);

    // Apply lifecycle effects to the canonical tab map under the lock so
    // subsequent reads (snapshot / events_clone / tab_ids_in_order) see the
    // post-event state. The widget then walks the returned `lifecycle` list
    // to drive STC page creation / teardown / badge updates.
    for (const auto& ev : lifecycle) {
        switch (ev.kind) {
            case LifeKind::sync_start: {
                auto it = tabs_.find(k_sync_id);
                if (it == tabs_.end()) break;
                TerminalTabState& tab = *it->second;
                tab.command         = ev.command;
                tab.active          = true;
                tab.killed          = false;
                tab.timed_out       = false;
                tab.exit_code       = 0;
                tab.parser.reset();
                tab.events.clear();
                tab.line_count      = 0;
                tab.stick_to_bottom = true;
                break;
            }
            case LifeKind::sync_exit: {
                auto it = tabs_.find(k_sync_id);
                if (it == tabs_.end()) break;
                TerminalTabState& tab = *it->second;
                tab.active    = false;
                tab.exit_code = ev.exit_code;
                tab.timed_out = ev.timed_out;
                break;
            }
            case LifeKind::bg_start: {
                ensure_tab_locked_(ev.id, ev.command);
                break;
            }
            case LifeKind::bg_exit: {
                // We match the S5.B widget contract: bg tabs are torn down
                // when the process exits. Erase from the map + order so the
                // widget rebuild on activation sees the closed set.
                auto it = tabs_.find(ev.id);
                if (it != tabs_.end()) tabs_.erase(it);
                tab_order_.erase(
                    std::remove(tab_order_.begin(), tab_order_.end(), ev.id),
                    tab_order_.end());
                if (active_sub_tab_id_ == ev.id) active_sub_tab_id_ = k_sync_id;
                break;
            }
        }
    }
    return true;
}

std::vector<AnsiEvent>
TerminalPanelState::parse_and_append(int id, const std::string& chunk)
{
    if (chunk.empty()) return {};
    std::lock_guard l(mu_);
    auto it = tabs_.find(id);
    if (it == tabs_.end()) return {};
    TerminalTabState& tab = *it->second;
    std::vector<AnsiEvent> events;
    tab.parser.consume(chunk.data(), chunk.size(), events);
    if (events.empty()) return events;

    // Count newlines in the new run so trim_locked_ can drop oldest events
    // when the per-tab cap is exceeded.
    for (const auto& ev : events) {
        if (ev.kind == AnsiEventKind::text) {
            for (char c : ev.text) {
                if (c == '\n') ++tab.line_count;
            }
        }
    }

    tab.events.insert(tab.events.end(), events.begin(), events.end());
    trim_locked_(tab);
    return events;
}

void TerminalPanelState::clear_log(int id)
{
    std::lock_guard l(mu_);
    auto it = tabs_.find(id);
    if (it == tabs_.end()) return;
    TerminalTabState& tab = *it->second;
    tab.events.clear();
    tab.line_count = 0;
    tab.parser.reset();
}

std::vector<int> TerminalPanelState::tab_ids_in_order() const
{
    std::lock_guard l(mu_);
    return tab_order_;
}

std::optional<TerminalPanelState::TabSnapshot>
TerminalPanelState::snapshot(int id) const
{
    std::lock_guard l(mu_);
    auto it = tabs_.find(id);
    if (it == tabs_.end()) return std::nullopt;
    const TerminalTabState& tab = *it->second;
    TabSnapshot s;
    s.id              = tab.id;
    s.command         = tab.command;
    s.active          = tab.active;
    s.exit_code       = tab.exit_code;
    s.killed          = tab.killed;
    s.timed_out       = tab.timed_out;
    s.stick_to_bottom = tab.stick_to_bottom;
    return s;
}

std::vector<AnsiEvent> TerminalPanelState::events_clone(int id) const
{
    std::lock_guard l(mu_);
    auto it = tabs_.find(id);
    if (it == tabs_.end()) return {};
    return it->second->events;
}

int TerminalPanelState::active_sub_tab_id() const
{
    std::lock_guard l(mu_);
    return active_sub_tab_id_;
}

void TerminalPanelState::set_active_sub_tab_id(int id)
{
    std::lock_guard l(mu_);
    if (tabs_.find(id) != tabs_.end()) active_sub_tab_id_ = id;
}

void TerminalPanelState::set_stick_to_bottom(int id, bool stick)
{
    std::lock_guard l(mu_);
    auto it = tabs_.find(id);
    if (it != tabs_.end()) it->second->stick_to_bottom = stick;
}

// -- internals (mu_ held) ----------------------------------------------------

TerminalTabState* TerminalPanelState::ensure_tab_locked_(int id,
                                                         const std::string& command)
{
    auto it = tabs_.find(id);
    if (it != tabs_.end()) {
        // Refresh command + activity flag if a previous bg_exit had erased it
        // and we re-saw a bg_start with the same id (won't happen with
        // monotonic registry ids, but cheap to be safe).
        it->second->command = command;
        it->second->active  = true;
        return it->second.get();
    }
    auto tab = std::make_unique<TerminalTabState>();
    tab->id              = id;
    tab->command         = command;
    tab->active          = true;
    tab->stick_to_bottom = true;
    TerminalTabState* raw = tab.get();
    tabs_[id] = std::move(tab);
    tab_order_.push_back(id);
    return raw;
}

void TerminalPanelState::trim_locked_(TerminalTabState& tab)
{
    if (max_lines_per_tab_ == 0) return;
    if (tab.line_count <= static_cast<int>(max_lines_per_tab_)) return;
    int target_drop = tab.line_count - static_cast<int>(max_lines_per_tab_);
    // Walk from the front dropping events until we've shed `target_drop`
    // newlines. We drop at event granularity -- an event that contains the
    // boundary line is dropped whole, which slightly over-trims (acceptable
    // when the cap is a soft scrollback bound).
    int dropped_lines  = 0;
    std::size_t drop_n = 0;
    for (; drop_n < tab.events.size() && dropped_lines < target_drop; ++drop_n) {
        const auto& ev = tab.events[drop_n];
        if (ev.kind == AnsiEventKind::text) {
            for (char c : ev.text) {
                if (c == '\n') ++dropped_lines;
            }
        }
    }
    if (drop_n == 0) return;
    tab.events.erase(tab.events.begin(), tab.events.begin() + drop_n);
    tab.line_count -= dropped_lines;
    if (tab.line_count < 0) tab.line_count = 0;
}

} // namespace locus
