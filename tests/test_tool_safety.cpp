// S5.O -- Tool & Filesystem Safety Hardening
//
// Covers three independent fixes bundled in one stage:
//   1. Path containment: component-aware is_inside / resolve_path now reject
//      the "/foo" vs "/foo-bar" prefix false positive a raw byte compare let
//      through. On Windows the check is case-insensitive.
//   2. Approval keying: ToolDispatcher::submit_decision is keyed by call_id
//      so a stale or late-delivered decision cannot wake the wrong dispatch.
//      Decisions submitted for a call_id with no waiter are dropped + warned.
//   3. write_file atomicity: WriteFileTool now writes through the shared
//      tools::write_atomic helper (same path edit_file already used) so a
//      crash between truncate and final flush can't leave a half-written or
//      zero-byte file on disk.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "tools/shared.h"
#include "tools/tool_registry.h"
#include "tools/file_tools.h"
#include "agent/tool_dispatcher.h"
#include "agent/activity_log.h"
#include "core/frontend.h"
#include "core/frontend_registry.h"
#include "support/fake_workspace_services.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

// -- 1. Path containment ----------------------------------------------------

TEST_CASE("is_inside: identical paths are inside", "[s5.o][safety][path]")
{
    REQUIRE(locus::tools::is_inside("D:/foo", "D:/foo"));
    REQUIRE(locus::tools::is_inside("D:/foo/", "D:/foo"));
    REQUIRE(locus::tools::is_inside("D:/foo", "D:/foo/"));
}

TEST_CASE("is_inside: strict descendant is inside", "[s5.o][safety][path]")
{
    REQUIRE(locus::tools::is_inside("D:/foo/bar.txt", "D:/foo"));
    REQUIRE(locus::tools::is_inside("D:/foo/sub/deeper/x.cpp", "D:/foo"));
}

TEST_CASE("is_inside: sibling with shared prefix is rejected", "[s5.o][safety][path]")
{
    // The bug this stage fixes: a raw byte-prefix compare would accept this.
    REQUIRE_FALSE(locus::tools::is_inside("D:/foo-bar/evil.txt", "D:/foo"));
    REQUIRE_FALSE(locus::tools::is_inside("D:/foob", "D:/foo"));
    REQUIRE_FALSE(locus::tools::is_inside("D:/foobar/x", "D:/foo"));
}

TEST_CASE("is_inside: parent traversal is rejected", "[s5.o][safety][path]")
{
    // lexically_normal resolves the "..".
    REQUIRE_FALSE(locus::tools::is_inside("D:/foo/../bar.txt", "D:/foo"));
    REQUIRE_FALSE(locus::tools::is_inside("D:/foo/../../etc/passwd", "D:/foo"));
}

TEST_CASE("is_inside: root deeper than candidate is rejected", "[s5.o][safety][path]")
{
    REQUIRE_FALSE(locus::tools::is_inside("D:/foo", "D:/foo/sub"));
}

#if defined(_WIN32)
TEST_CASE("is_inside: case-insensitive on Windows", "[s5.o][safety][path]")
{
    REQUIRE(locus::tools::is_inside("D:/Foo/bar.txt", "D:/foo"));
    REQUIRE(locus::tools::is_inside("d:/foo/BAR.txt", "D:/Foo"));
    REQUIRE(locus::tools::is_inside("C:/Users/X/proj/file", "c:/users/x/proj"));
    // Different drive letter is still rejected -- different first component.
    REQUIRE_FALSE(locus::tools::is_inside("E:/foo/bar", "D:/foo"));
}
#endif

TEST_CASE("is_inside: forward / back slash mix normalises", "[s5.o][safety][path]")
{
    // path::lexically_normal handles separators on each platform; this is
    // mostly belt-and-braces.
    REQUIRE(locus::tools::is_inside(fs::path("D:/foo") / "bar/baz.txt",
                                    fs::path("D:/foo")));
}

TEST_CASE("resolve_path: sibling with shared prefix is rejected", "[s5.o][safety][path]")
{
    auto tmp = fs::temp_directory_path() / "locus_s5o_resolve";
    fs::create_directories(tmp / "foo");
    fs::create_directories(tmp / "foo-bar");
    {
        std::ofstream(tmp / "foo-bar" / "evil.txt") << "x";
    }

    locus::test::FakeWorkspaceServices ws{tmp / "foo"};
    // Try to escape via "../foo-bar/evil.txt" -- canonicalises to the sibling
    // and is rejected. (Pre-S5.O this passed a byte-prefix check on some
    // platforms because "foo-bar" starts with "foo".)
    auto p = locus::tools::resolve_path(ws, "../foo-bar/evil.txt");
    REQUIRE(p.empty());

    fs::remove_all(tmp);
}

TEST_CASE("resolve_path: in-workspace path resolves", "[s5.o][safety][path]")
{
    auto tmp = fs::temp_directory_path() / "locus_s5o_resolve_ok";
    fs::create_directories(tmp / "foo");
    {
        std::ofstream(tmp / "foo" / "hello.txt") << "hi";
    }

    locus::test::FakeWorkspaceServices ws{tmp / "foo"};
    auto p = locus::tools::resolve_path(ws, "hello.txt");
    REQUIRE_FALSE(p.empty());
    REQUIRE(fs::exists(p));

    fs::remove_all(tmp);
}

// -- 2. Approval decision keyed by call_id ----------------------------------

namespace {

// Minimal frontend that records every event. ToolDispatcher's broadcasts
// don't matter for the keying assertions, but we still need a registry.
class RecorderFrontend : public locus::IFrontend {
public:
    void on_turn_start() override {}
    void on_token(std::string_view) override {}
    void on_tool_call_pending(const locus::ToolCall&,
                              const std::string&,
                              bool,
                              const std::vector<std::string>&) override {}
    void on_tool_result(const std::string& id,
                        const std::string&,
                        bool success) override
    {
        std::lock_guard lock(mu_);
        results.push_back({id, success});
    }
    void on_turn_complete() override {}
    void on_context_meter(int, int, int, int) override {}
    void on_compaction_needed(int, int) override {}
    void on_session_reset() override {}
    void on_error(const std::string&) override {}
    void on_embedding_progress(int, int) override {}
    void on_activity(const locus::ActivityEvent&) override {}

    struct Result { std::string id; bool success; };
    std::vector<Result> results;
    std::mutex          mu_;
};

// Tool whose only job is to record that execute() ran for the given call id.
class TraceTool : public locus::ITool {
public:
    explicit TraceTool(std::string name) : name_(std::move(name)) {}

    std::string                       name()        const override { return name_; }
    std::string                       description() const override { return "trace"; }
    std::vector<locus::ToolParam>     params()      const override { return {}; }
    locus::ToolApprovalPolicy approval_policy() const override
    {
        return locus::ToolApprovalPolicy::ask;
    }

    locus::ToolResult execute(const locus::ToolCall& call,
                              locus::IWorkspaceServices&) override
    {
        std::lock_guard lock(mu_);
        executed_ids.push_back(call.id);
        return {true, "ok-" + call.id, "ok-" + call.id};
    }

    std::vector<std::string> executed_ids;
    std::mutex               mu_;

private:
    std::string name_;
};

} // namespace

TEST_CASE("ToolDispatcher: decision is consumed only by matching call_id",
          "[s5.o][safety][approval]")
{
    auto tmp = fs::temp_directory_path() / "locus_s5o_dispatch";
    fs::create_directories(tmp);

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::FrontendRegistry frontends;
    RecorderFrontend rec;
    frontends.register_frontend(&rec);

    locus::ActivityLog activity{frontends};
    locus::ToolRegistry tools;
    auto* trace = new TraceTool("trace");
    tools.register_tool(std::unique_ptr<locus::ITool>(trace));

    std::atomic<bool> cancel{false};
    locus::ToolDispatcher dispatcher{tools, ws, activity, frontends, cancel};

    // Two dispatches in flight, each on its own thread, both parked on the
    // approval gate. We submit a decision for call B first and assert that
    // dispatch B (only) wakes; dispatch A keeps waiting.

    std::vector<locus::ChatMessage> appended;
    std::mutex appended_mu;
    auto append = [&](locus::ChatMessage m) {
        std::lock_guard lock(appended_mu);
        appended.push_back(std::move(m));
    };

    std::thread tA([&] {
        locus::ToolCall ca{"call_A", "trace", {}};
        dispatcher.dispatch(ca, append);
    });
    std::thread tB([&] {
        locus::ToolCall cb{"call_B", "trace", {}};
        dispatcher.dispatch(cb, append);
    });

    // Give both threads a moment to park on the approval cv.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Decision for B only.
    dispatcher.submit_decision("call_B", locus::ToolDecision::approve);

    // B should finish quickly; A must still be parked.
    tB.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    {
        std::lock_guard lock(trace->mu_);
        REQUIRE(trace->executed_ids.size() == 1);
        REQUIRE(trace->executed_ids[0] == "call_B");
    }

    // Now wake A with its own decision.
    dispatcher.submit_decision("call_A", locus::ToolDecision::approve);
    tA.join();

    {
        std::lock_guard lock(trace->mu_);
        REQUIRE(trace->executed_ids.size() == 2);
        // Order is whichever dispatch wakes first, but the set is {A, B}.
        std::set<std::string> ids(trace->executed_ids.begin(),
                                  trace->executed_ids.end());
        REQUIRE(ids.count("call_A") == 1);
        REQUIRE(ids.count("call_B") == 1);
    }

    fs::remove_all(tmp);
}

TEST_CASE("ToolDispatcher: stale decision for unknown call_id is dropped",
          "[s5.o][safety][approval]")
{
    auto tmp = fs::temp_directory_path() / "locus_s5o_dispatch_stale";
    fs::create_directories(tmp);

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::FrontendRegistry frontends;
    RecorderFrontend rec;
    frontends.register_frontend(&rec);

    locus::ActivityLog activity{frontends};
    locus::ToolRegistry tools;
    auto* trace = new TraceTool("trace");
    tools.register_tool(std::unique_ptr<locus::ITool>(trace));

    std::atomic<bool> cancel{false};
    locus::ToolDispatcher dispatcher{tools, ws, activity, frontends, cancel};

    // Submit a decision for a call_id no dispatch is waiting on. Then start
    // a real dispatch for call_X and verify it still parks (the stale
    // decision was dropped, not held).
    dispatcher.submit_decision("call_stale", locus::ToolDecision::approve);

    std::vector<locus::ChatMessage> appended;
    std::mutex appended_mu;
    auto append = [&](locus::ChatMessage m) {
        std::lock_guard lock(appended_mu);
        appended.push_back(std::move(m));
    };

    std::thread tX([&] {
        locus::ToolCall cx{"call_X", "trace", {}};
        dispatcher.dispatch(cx, append);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard lock(trace->mu_);
        REQUIRE(trace->executed_ids.empty());  // still waiting
    }

    // Now feed the right decision and let it finish.
    dispatcher.submit_decision("call_X", locus::ToolDecision::approve);
    tX.join();

    {
        std::lock_guard lock(trace->mu_);
        REQUIRE(trace->executed_ids.size() == 1);
        REQUIRE(trace->executed_ids[0] == "call_X");
    }

    fs::remove_all(tmp);
}

// -- 3. write_file atomicity ------------------------------------------------

TEST_CASE("write_atomic: happy path leaves no tmp behind", "[s5.o][safety][atomic]")
{
    auto tmp = fs::temp_directory_path() / "locus_s5o_atomic_ok";
    fs::create_directories(tmp);
    auto target = tmp / "out.txt";

    auto err = locus::tools::write_atomic(target, "fresh-content");
    REQUIRE(err.empty());
    REQUIRE(fs::exists(target));

    std::string body;
    {
        std::ifstream in(target);
        std::ostringstream ss; ss << in.rdbuf();
        body = ss.str();
    }
    REQUIRE(body == "fresh-content");

    // No <target>.locus-tmp lingers.
    auto tmpsidecar = target;
    tmpsidecar += ".locus-tmp";
    REQUIRE_FALSE(fs::exists(tmpsidecar));

    fs::remove_all(tmp);
}

TEST_CASE("write_atomic: overwrite preserves previous content on temp-create failure",
          "[s5.o][safety][atomic]")
{
    auto tmp = fs::temp_directory_path() / "locus_s5o_atomic_fail";
    fs::create_directories(tmp);
    auto target = tmp / "out.txt";
    {
        std::ofstream out(target);
        out << "ORIGINAL";
    }

    // Pre-create a directory at the temp path -- writing to it as a file will
    // fail at the ofstream open step, exiting write_atomic before any rename
    // can clobber the target. (Simulates a process kill / disk-full case
    // where the temp body never lands.)
    auto tmppath = target;
    tmppath += ".locus-tmp";
    fs::create_directory(tmppath);

    auto err = locus::tools::write_atomic(target, "NEW");
    REQUIRE_FALSE(err.empty());  // error reported

    // Target still has the original bytes.
    std::string body;
    {
        std::ifstream in(target);
        std::ostringstream ss; ss << in.rdbuf();
        body = ss.str();
    }
    REQUIRE(body == "ORIGINAL");

    fs::remove_all(tmp);
}

TEST_CASE("WriteFileTool: overwrite is atomic (no tmp lingers)", "[s5.o][safety][atomic]")
{
    auto tmp = fs::temp_directory_path() / "locus_s5o_write_tool";
    fs::create_directories(tmp);
    auto target = tmp / "data.txt";
    {
        std::ofstream out(target);
        out << "OLD";
    }

    locus::test::FakeWorkspaceServices ws{tmp};
    locus::WriteFileTool tool;
    locus::ToolCall call{"c1", "write_file",
        {{"path", "data.txt"}, {"content", "NEW"}, {"overwrite", true}}};
    auto result = tool.execute(call, ws);
    REQUIRE(result.success);

    std::string body;
    {
        std::ifstream in(target);
        std::ostringstream ss; ss << in.rdbuf();
        body = ss.str();
    }
    REQUIRE(body == "NEW");

    auto tmpsidecar = target;
    tmpsidecar += ".locus-tmp";
    REQUIRE_FALSE(fs::exists(tmpsidecar));

    fs::remove_all(tmp);
}
