// S5.R per-tab terminal panel -- unit tests against the wx-free model
// (TerminalPanelState + ProcessSinkBroker). The widget side is exercised
// by the UI automation script tests/ui_automation/scripts/terminal_per_tab.json.

#include <catch2/catch_test_macros.hpp>

#include "frontends/gui/terminal_panel_state.h"
#include "tools/process_sink.h"

#include <string>
#include <vector>

using namespace locus;

namespace {

// Drives the IProcessSink writer/reader pair end-to-end without a thread.
std::string events_to_text(const std::vector<AnsiEvent>& events)
{
    std::string out;
    for (const auto& ev : events) {
        if (ev.kind == AnsiEventKind::text) out += ev.text;
    }
    return out;
}

std::string state_text(const TerminalPanelState& state, int sub_tab_id)
{
    return events_to_text(state.events_clone(sub_tab_id));
}

void drive_and_apply(TerminalPanelState& state)
{
    // Simulates the widget's drain + parse_and_append loop.
    std::deque<TerminalPanelState::LifeEvent> lifecycle;
    std::unordered_map<int, std::string> text;
    state.drain_pending(lifecycle, text);
    for (auto& [id, blob] : text) {
        if (!blob.empty()) state.parse_and_append(id, blob);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// TerminalPanelState lifecycle + chunk append
// ---------------------------------------------------------------------------

TEST_CASE("TerminalPanelState: sync flow appends events + sets badge",
          "[s5.r][terminal]")
{
    TerminalPanelState state;
    state.on_sync_started("echo hello");
    state.on_sync_chunk("hello\n", 6);
    state.on_sync_exited(0, /*timed_out=*/false);

    drive_and_apply(state);

    auto snap = state.snapshot(TerminalPanelState::k_sync_id);
    REQUIRE(snap.has_value());
    CHECK(snap->command == "echo hello");
    CHECK_FALSE(snap->active);
    CHECK(snap->exit_code == 0);

    CHECK(state_text(state, TerminalPanelState::k_sync_id) == "hello\n");
}

TEST_CASE("TerminalPanelState: bg start adds tab, bg exit removes it",
          "[s5.r][terminal]")
{
    TerminalPanelState state;
    state.on_bg_started(7, "sleep 1");
    state.on_bg_chunk(7, "ok\n", 3);

    drive_and_apply(state);
    auto ids_before = state.tab_ids_in_order();
    REQUIRE(ids_before.size() == 2);  // Run + bg id 7
    CHECK(ids_before[0] == TerminalPanelState::k_sync_id);
    CHECK(ids_before[1] == 7);
    CHECK(state_text(state, 7) == "ok\n");

    state.on_bg_exited(7, 0, /*killed=*/false);
    drive_and_apply(state);

    auto ids_after = state.tab_ids_in_order();
    REQUIRE(ids_after.size() == 1);  // bg gone
    CHECK(ids_after[0] == TerminalPanelState::k_sync_id);
    CHECK_FALSE(state.snapshot(7).has_value());
}

TEST_CASE("TerminalPanelState: sync_start resets the Run tab event log",
          "[s5.r][terminal]")
{
    TerminalPanelState state;
    state.on_sync_started("cmd1");
    state.on_sync_chunk("first\n", 6);
    drive_and_apply(state);
    REQUIRE(state_text(state, TerminalPanelState::k_sync_id) == "first\n");

    state.on_sync_started("cmd2");  // fresh invocation
    state.on_sync_chunk("second\n", 7);
    drive_and_apply(state);

    CHECK(state_text(state, TerminalPanelState::k_sync_id) == "second\n");
}

// ---------------------------------------------------------------------------
// Per-tab broker isolation
// ---------------------------------------------------------------------------

TEST_CASE("Per-tab broker isolation: chunks on broker A don't reach state B",
          "[s5.r][terminal][broker]")
{
    TerminalPanelState state_a;
    TerminalPanelState state_b;
    ProcessSinkBroker broker_a;
    ProcessSinkBroker broker_b;
    broker_a.add_sink(&state_a);
    broker_b.add_sink(&state_b);

    broker_a.emit_sync_started("ls");
    broker_a.emit_sync_chunk("A\n", 2);
    broker_b.emit_sync_started("pwd");
    broker_b.emit_sync_chunk("B\n", 2);

    drive_and_apply(state_a);
    drive_and_apply(state_b);

    CHECK(state_text(state_a, TerminalPanelState::k_sync_id) == "A\n");
    CHECK(state_text(state_b, TerminalPanelState::k_sync_id) == "B\n");

    broker_a.remove_sink(&state_a);
    broker_b.remove_sink(&state_b);
}

// ---------------------------------------------------------------------------
// Multi-subscriber broker
// ---------------------------------------------------------------------------

namespace {

struct CountingSink : public IProcessSink {
    int bg_started_calls   = 0;
    int bg_chunk_calls     = 0;
    int bg_exited_calls    = 0;
    int sync_started_calls = 0;

    void on_bg_started(int, const std::string&) override { ++bg_started_calls; }
    void on_bg_chunk  (int, const char*, std::size_t) override { ++bg_chunk_calls; }
    void on_bg_exited (int, int, bool) override { ++bg_exited_calls; }
    void on_sync_started(const std::string&) override { ++sync_started_calls; }
    void on_sync_chunk  (const char*, std::size_t) override {}
    void on_sync_exited (int, bool) override {}
};

} // namespace

TEST_CASE("ProcessSinkBroker: multi-subscriber fan-out + remove",
          "[s5.r][terminal][broker]")
{
    ProcessSinkBroker broker;
    CountingSink sink_a;
    CountingSink sink_b;

    broker.add_sink(&sink_a);
    CHECK(broker.subscriber_count() == 1);

    broker.emit_bg_started(1, "cmd");
    CHECK(sink_a.bg_started_calls == 1);
    CHECK(sink_b.bg_started_calls == 0);

    broker.add_sink(&sink_b);
    CHECK(broker.subscriber_count() == 2);

    broker.emit_bg_chunk(1, "x", 1);
    CHECK(sink_a.bg_chunk_calls == 1);
    CHECK(sink_b.bg_chunk_calls == 1);

    broker.remove_sink(&sink_a);
    CHECK(broker.subscriber_count() == 1);

    broker.emit_bg_exited(1, 0, false);
    CHECK(sink_a.bg_exited_calls == 0);  // unsubscribed
    CHECK(sink_b.bg_exited_calls == 1);

    broker.remove_sink(&sink_b);
    CHECK(broker.subscriber_count() == 0);

    // No subscribers -- emits become no-ops.
    broker.emit_sync_started("noop");
    CHECK(sink_a.sync_started_calls == 0);
    CHECK(sink_b.sync_started_calls == 0);
}

TEST_CASE("ProcessSinkBroker: add_sink is idempotent",
          "[s5.r][terminal][broker]")
{
    ProcessSinkBroker broker;
    CountingSink sink;
    broker.add_sink(&sink);
    broker.add_sink(&sink);  // duplicate ignored
    CHECK(broker.subscriber_count() == 1);
    broker.emit_sync_started("hello");
    CHECK(sink.sync_started_calls == 1);
}

// ---------------------------------------------------------------------------
// Replay-on-activation
// ---------------------------------------------------------------------------

TEST_CASE("events_clone returns the full canonical log for replay",
          "[s5.r][terminal]")
{
    TerminalPanelState state;
    state.on_sync_started("cmd");
    state.on_sync_chunk("line 1\n", 7);
    state.on_sync_chunk("line 2\n", 7);
    drive_and_apply(state);

    auto events = state.events_clone(TerminalPanelState::k_sync_id);
    CHECK(events_to_text(events) == "line 1\nline 2\n");

    // Clone is independent -- mutating doesn't affect the state.
    events.clear();
    CHECK(state_text(state, TerminalPanelState::k_sync_id) == "line 1\nline 2\n");
}

// ---------------------------------------------------------------------------
// Scrollback trim
// ---------------------------------------------------------------------------

TEST_CASE("Scrollback cap drops oldest events",
          "[s5.r][terminal]")
{
    // Trim drops at event granularity -- a single huge text event is dropped
    // whole. Pump lines as separate chunks (with a drain between each) so
    // each produces its own text event and the trim has fine-grained units
    // to operate on.
    TerminalPanelState state(/*max_lines_per_tab=*/5);
    state.on_sync_started("seq");
    drive_and_apply(state);
    for (int i = 0; i < 12; ++i) {
        std::string line = "line " + std::to_string(i) + "\n";
        state.on_sync_chunk(line.data(), line.size());
        drive_and_apply(state);
    }

    auto text = state_text(state, TerminalPanelState::k_sync_id);
    int newlines = 0;
    for (char c : text) if (c == '\n') ++newlines;
    // Cap is 5; trim drops at event granularity, so we may keep slightly
    // fewer than 5 lines after trim, but never more.
    CHECK(newlines <= 5);
    CHECK(newlines >= 1);
    // Whatever we retain must start at some `line N` with N >= 12-newlines
    // (the oldest possible kept). With per-chunk drain producing one event
    // per line, the trim's per-event granularity matches per-line and we
    // expect the retained text to begin around line 7 or later.
    REQUIRE(text.size() > 5);
    CHECK(text.substr(0, 5) == "line ");
    char first_digit = text[5];
    int first_line = first_digit - '0';
    CHECK(first_line >= 12 - newlines);
}

// ---------------------------------------------------------------------------
// Auto-show predicate (pure)
// ---------------------------------------------------------------------------

TEST_CASE("should_auto_show_terminal predicate",
          "[s5.r][terminal][auto_show]")
{
    // event from non-active tab -> never
    CHECK_FALSE(should_auto_show_terminal(/*evt=*/1, /*active=*/2,
                                            /*pane_hidden=*/true,
                                            /*user_hid=*/false));
    // pane already shown -> never
    CHECK_FALSE(should_auto_show_terminal(1, 1, /*pane_hidden=*/false, false));
    // user has hidden since last show -> never
    CHECK_FALSE(should_auto_show_terminal(1, 1, true, /*user_hid=*/true));
    // active tab, pane hidden, user hasn't hidden -> show
    CHECK(should_auto_show_terminal(1, 1, true, false));
}

// ---------------------------------------------------------------------------
// State snapshot fields
// ---------------------------------------------------------------------------

TEST_CASE("Snapshot reflects exit, killed, timed_out flags",
          "[s5.r][terminal]")
{
    TerminalPanelState state;
    state.on_bg_started(3, "long");
    drive_and_apply(state);
    {
        auto snap = state.snapshot(3);
        REQUIRE(snap.has_value());
        CHECK(snap->active);
    }
    state.on_bg_exited(3, 137, /*killed=*/true);
    drive_and_apply(state);
    // The tab is now closed.
    CHECK_FALSE(state.snapshot(3).has_value());

    // sync timeout path
    state.on_sync_started("hang");
    state.on_sync_exited(-1, /*timed_out=*/true);
    drive_and_apply(state);
    auto snap = state.snapshot(TerminalPanelState::k_sync_id);
    REQUIRE(snap.has_value());
    CHECK_FALSE(snap->active);
    CHECK(snap->timed_out);
    CHECK(snap->exit_code == -1);
}
