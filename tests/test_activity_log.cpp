// ActivityLog: consecutive index_event coalescing.
//
// The activity panel got buried under "Indexed 1 file" / "Embedding
// progress: ..." rows fired by the file watcher and embedding worker. The
// fix coalesces consecutive index_event entries that share a category
// prefix (the substring before the first digit in the summary) into a
// single row -- summary, detail and timestamp update in place; id stays
// stable so the frontend can find and refresh the existing widget.
//
// These tests pin the rules so a future refactor doesn't regress them.

#include "agent/activity_log.h"
#include "core/activity_event.h"
#include "core/frontend.h"
#include "core/frontend_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace locus;

namespace {

// Minimal IFrontend that just records every callback fan-out from the log.
struct Recorder : IFrontend {
    std::vector<ActivityEvent> appended;
    std::vector<ActivityEvent> updated;

    void on_turn_start() override {}
    void on_token(std::string_view) override {}
    void on_tool_call_pending(const ToolCall&, const std::string&, bool,
                              const std::vector<std::string>&) override {}
    void on_tool_result(const std::string&, const std::string&, bool) override {}
    void on_turn_complete() override {}
    void on_context_meter(int, int, int, int, int, long long) override {}
    void on_compaction_needed(int, int) override {}
    void on_session_reset() override {}
    void on_error(const std::string&) override {}
    void on_embedding_progress(int, int) override {}
    void on_activity(const ActivityEvent& e) override { appended.push_back(e); }
    void on_activity_updated(const ActivityEvent& e) override { updated.push_back(e); }
};

} // namespace

TEST_CASE("ActivityLog: two consecutive index_events with same prefix coalesce",
          "[activity-log][s5.m][coalesce]")
{
    FrontendRegistry registry;
    Recorder r;
    registry.register_frontend(&r);
    ActivityLog log(registry);

    log.emit_index_event("Indexed 1 file",  "+ a.cpp");
    log.emit_index_event("Indexed 5 files", "+ b.cpp\n+ c.cpp");

    // get_since returns the on-disk buffer state; coalesced -> one row.
    auto buf = log.get_since(0);
    REQUIRE(buf.size() == 1);
    REQUIRE(buf[0].summary == "Indexed 5 files");
    REQUIRE(buf[0].detail  == "+ b.cpp\n+ c.cpp");

    // First emit broadcasts on_activity; second emit broadcasts
    // on_activity_updated with the same id.
    REQUIRE(r.appended.size() == 1);
    REQUIRE(r.updated.size()  == 1);
    REQUIRE(r.appended[0].id  == r.updated[0].id);
    REQUIRE(r.updated[0].summary == "Indexed 5 files");
}

TEST_CASE("ActivityLog: different category prefixes do not coalesce",
          "[activity-log][s5.m][coalesce]")
{
    FrontendRegistry registry;
    Recorder r;
    registry.register_frontend(&r);
    ActivityLog log(registry);

    log.emit_index_event("Indexed 1 file",          "");
    log.emit_index_event("Embedding progress: 5/10", "");

    auto buf = log.get_since(0);
    REQUIRE(buf.size() == 2);
    REQUIRE(buf[0].summary == "Indexed 1 file");
    REQUIRE(buf[1].summary == "Embedding progress: 5/10");
    REQUIRE(buf[0].id != buf[1].id);

    REQUIRE(r.appended.size() == 2);
    REQUIRE(r.updated.empty());
}

TEST_CASE("ActivityLog: index_event after non-index_event does not coalesce",
          "[activity-log][s5.m][coalesce]")
{
    FrontendRegistry registry;
    Recorder r;
    registry.register_frontend(&r);
    ActivityLog log(registry);

    log.emit(ActivityKind::tool_call, "edit_file", "");
    log.emit_index_event("Indexed 1 file", "");

    auto buf = log.get_since(0);
    REQUIRE(buf.size() == 2);
    REQUIRE(buf[0].kind == ActivityKind::tool_call);
    REQUIRE(buf[1].kind == ActivityKind::index_event);

    REQUIRE(r.appended.size() == 2);
    REQUIRE(r.updated.empty());
}

TEST_CASE("ActivityLog: non-index_event after index_event does not coalesce",
          "[activity-log][s5.m][coalesce]")
{
    FrontendRegistry registry;
    Recorder r;
    registry.register_frontend(&r);
    ActivityLog log(registry);

    log.emit_index_event("Indexed 1 file", "");
    log.emit(ActivityKind::user_message, "hello", "");

    auto buf = log.get_since(0);
    REQUIRE(buf.size() == 2);
    REQUIRE(buf[0].kind == ActivityKind::index_event);
    REQUIRE(buf[1].kind == ActivityKind::user_message);

    REQUIRE(r.appended.size() == 2);
    REQUIRE(r.updated.empty());
}

TEST_CASE("ActivityLog: three same-prefix index_events collapse to one",
          "[activity-log][s5.m][coalesce]")
{
    FrontendRegistry registry;
    Recorder r;
    registry.register_frontend(&r);
    ActivityLog log(registry);

    log.emit_index_event("Indexed 1 file",  "a");
    log.emit_index_event("Indexed 2 files", "b");
    log.emit_index_event("Indexed 7 files", "c");

    auto buf = log.get_since(0);
    REQUIRE(buf.size() == 1);
    REQUIRE(buf[0].summary == "Indexed 7 files");
    REQUIRE(buf[0].detail  == "c");

    REQUIRE(r.appended.size() == 1);
    REQUIRE(r.updated.size()  == 2);  // second + third emits both update
    REQUIRE(r.appended[0].id == r.updated.back().id);
}
