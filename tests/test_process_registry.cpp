#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "process_registry.h"
#include "tool.h"
#include "tools/process_tools.h"
#include "support/fake_workspace_services.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using namespace std::chrono_literals;

#ifdef _WIN32

namespace {

// Service shim that exposes a real ProcessRegistry to the bg tools.
class WsWithProcs : public locus::IWorkspaceServices {
public:
    WsWithProcs(fs::path root, std::size_t cap)
        : root_(std::move(root)), procs_(root_, cap) {}

    const fs::path&         root() const override   { return root_; }
    locus::IndexQuery*      index() override        { return nullptr; }
    locus::ProcessRegistry* processes() override    { return &procs_; }

private:
    fs::path                root_;
    locus::ProcessRegistry  procs_;
};

fs::path tmp_root()
{
    auto p = fs::temp_directory_path() / "locus_test_processes";
    fs::create_directories(p);
    return p;
}

// Block until `predicate()` returns true or `timeout` elapses.
template <typename F>
bool wait_for(F predicate, std::chrono::milliseconds timeout = 5000ms)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

} // namespace

TEST_CASE("ProcessRegistry: spawn returns id and process exits cleanly", "[s4.i]")
{
    locus::ProcessRegistry reg(tmp_root(), 64 * 1024);
    int id = reg.spawn("echo hello");
    REQUIRE(id > 0);

    REQUIRE(wait_for([&] {
        auto entries = reg.list();
        return !entries.empty() &&
               entries[0].status != locus::BackgroundProcess::Status::running;
    }));

    auto entries = reg.list();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].id == id);
    REQUIRE(entries[0].status == locus::BackgroundProcess::Status::exited);
    REQUIRE(entries[0].exit_code == 0);

    auto out = reg.read_output(id, std::nullopt);
    REQUIRE(out.has_value());
    REQUIRE_THAT(out->data, ContainsSubstring("hello"));
}

TEST_CASE("ProcessRegistry: read_output uses last-delivered offset by default", "[s4.i]")
{
    locus::ProcessRegistry reg(tmp_root(), 64 * 1024);
    int id = reg.spawn("echo first && echo second");

    REQUIRE(wait_for([&] {
        auto e = reg.list();
        return !e.empty() && e[0].status != locus::BackgroundProcess::Status::running;
    }));

    auto first = reg.read_output(id, std::nullopt);
    REQUIRE(first.has_value());
    REQUIRE_THAT(first->data, ContainsSubstring("first"));
    REQUIRE_THAT(first->data, ContainsSubstring("second"));
    REQUIRE(first->next_offset > 0);

    // A second read with no offset returns nothing new.
    auto second = reg.read_output(id, std::nullopt);
    REQUIRE(second.has_value());
    REQUIRE(second->data.empty());

    // Explicit since_offset=0 returns the full buffer regardless.
    auto from_zero = reg.read_output(id, std::size_t{0});
    REQUIRE(from_zero.has_value());
    REQUIRE_THAT(from_zero->data, ContainsSubstring("first"));
}

TEST_CASE("ProcessRegistry: ring buffer drops bytes on overflow", "[s4.i]")
{
    // 64-byte cap: anything past that is reported as dropped.
    locus::ProcessRegistry reg(tmp_root(), 64);
    int id = reg.spawn("for /L %i in (1,1,200) do @echo line_%i");

    REQUIRE(wait_for([&] {
        auto e = reg.list();
        return !e.empty() && e[0].status != locus::BackgroundProcess::Status::running;
    }, 15000ms));

    auto r = reg.read_output(id, std::size_t{0});
    REQUIRE(r.has_value());
    // Cap is honored: stored slice never exceeds the ring buffer cap.
    REQUIRE(r->data.size() <= 64);
    // Caller asked from byte 0; the registry has dropped older bytes — surface that.
    REQUIRE(r->dropped_before_window > 0);
    // next_offset is the running monotonic byte count; way past 64 here.
    REQUIRE(r->next_offset > 64);
}

TEST_CASE("ProcessRegistry: stop terminates a long-running process", "[s4.i]")
{
    locus::ProcessRegistry reg(tmp_root(), 64 * 1024);
    int id = reg.spawn("ping -n 60 -w 1000 127.0.0.1");

    // Give the OS a beat to spawn before we kill it.
    std::this_thread::sleep_for(200ms);
    REQUIRE(reg.stop(id));

    REQUIRE(wait_for([&] {
        auto e = reg.list();
        return !e.empty() && e[0].status != locus::BackgroundProcess::Status::running;
    }, 5000ms));

    auto entries = reg.list();
    REQUIRE(entries[0].status == locus::BackgroundProcess::Status::killed);
}

TEST_CASE("ProcessRegistry: dtor terminates surviving processes", "[s4.i]")
{
    int id = 0;
    {
        locus::ProcessRegistry reg(tmp_root(), 64 * 1024);
        id = reg.spawn("ping -n 60 -w 1000 127.0.0.1");
        REQUIRE(id > 0);
        // Drop without explicitly stopping — dtor must not hang.
    }
    // If we got here without timing out, the dtor terminated and joined OK.
    SUCCEED();
}

TEST_CASE("ProcessRegistry: read_output / stop on unknown id returns nothing", "[s4.i]")
{
    locus::ProcessRegistry reg(tmp_root(), 64 * 1024);
    REQUIRE_FALSE(reg.read_output(999, std::nullopt).has_value());
    REQUIRE_FALSE(reg.stop(999));
    REQUIRE_FALSE(reg.remove(999));
}

// -- Tools ------------------------------------------------------------------

TEST_CASE("RunCommandBgTool + ReadProcessOutputTool roundtrip", "[s4.i]")
{
    WsWithProcs ws(tmp_root(), 64 * 1024);

    locus::RunCommandBgTool start;
    locus::ToolCall call_start{"c1", "run_command_bg", {{"command", "echo from_bg"}}};
    auto r = start.execute(call_start, ws);
    REQUIRE(r.success);
    auto j = nlohmann::json::parse(r.content);
    int pid = j["process_id"].get<int>();
    REQUIRE(pid > 0);

    REQUIRE(wait_for([&] {
        auto e = ws.processes()->list();
        return !e.empty() && e[0].status != locus::BackgroundProcess::Status::running;
    }));

    locus::ReadProcessOutputTool read;
    locus::ToolCall call_read{"c2", "read_process_output", {{"process_id", pid}}};
    auto rr = read.execute(call_read, ws);
    REQUIRE(rr.success);
    auto rj = nlohmann::json::parse(rr.content);
    REQUIRE_THAT(rj["data"].get<std::string>(), ContainsSubstring("from_bg"));
    REQUIRE(rj["status"].get<std::string>() == "exited");
    REQUIRE(rj["exit_code"].get<int>() == 0);
}

TEST_CASE("ListProcessesTool reports pending bytes and exit code", "[s4.i]")
{
    WsWithProcs ws(tmp_root(), 64 * 1024);
    locus::RunCommandBgTool start;
    locus::ToolCall c{"x", "run_command_bg", {{"command", "echo aa && echo bb"}}};
    auto sr = start.execute(c, ws);
    REQUIRE(sr.success);

    REQUIRE(wait_for([&] {
        auto e = ws.processes()->list();
        return !e.empty() && e[0].status != locus::BackgroundProcess::Status::running;
    }));

    locus::ListProcessesTool list;
    auto lr = list.execute({"y", "list_processes", nlohmann::json::object()}, ws);
    REQUIRE(lr.success);
    auto lj = nlohmann::json::parse(lr.content);
    REQUIRE(lj.is_array());
    REQUIRE(lj.size() == 1);
    REQUIRE(lj[0]["status"].get<std::string>() == "exited");
    REQUIRE(lj[0]["bytes_pending"].get<std::size_t>() > 0);
}

TEST_CASE("Bg tools report unavailable when registry missing", "[s4.i]")
{
    locus::test::FakeWorkspaceServices ws(tmp_root());
    locus::RunCommandBgTool start;
    REQUIRE_FALSE(start.available(ws));

    auto r = start.execute({"c", "run_command_bg", {{"command", "echo nope"}}}, ws);
    REQUIRE_FALSE(r.success);
    REQUIRE_THAT(r.content, ContainsSubstring("not available"));
}

#endif // _WIN32
