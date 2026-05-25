// Session save/restore -- round-trip parity for the parts of agent state we
// promise to persist:
//   * full ChatMessage fields (content + reasoning_content + tool_calls +
//     tool_call_id)
//   * top-level agent_mode + plan + plan_awaiting_decision extras
//   * activity sidecar JSONL (set_persist_path / replay_from) -- opt-in
//   * SessionManager::remove() also removes the activity sidecar
//
// These tests are deterministic (no live LLM, no Workspace, no AgentCore).
// The full live-LLM round trip is covered by the matching integration test
// at tests/integration/test_int_session_restore.cpp.

#include <catch2/catch_test_macros.hpp>

#include "agent/activity_log.h"
#include "agent/agent_mode.h"
#include "agent/conversation.h"
#include "agent/session_manager.h"
#include "core/activity_event.h"
#include "core/frontend.h"
#include "core/frontend_registry.h"
#include "llm/llm_client.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

using namespace locus;
namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(const std::string& tag)
{
    auto base = fs::temp_directory_path() / ("locus_restore_" + tag + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base);
    return base;
}

ChatMessage make_user(const std::string& content)
{
    ChatMessage m;
    m.role    = MessageRole::user;
    m.content = content;
    return m;
}

ChatMessage make_assistant_with_thinking(const std::string& content,
                                         const std::string& thinking)
{
    ChatMessage m;
    m.role               = MessageRole::assistant;
    m.content            = content;
    m.reasoning_content  = thinking;
    return m;
}

ChatMessage make_assistant_with_tool_call(const std::string& call_id,
                                          const std::string& name,
                                          const std::string& args)
{
    ChatMessage m;
    m.role = MessageRole::assistant;
    ToolCallRequest req;
    req.id        = call_id;
    req.name      = name;
    req.arguments = args;
    m.tool_calls.push_back(std::move(req));
    return m;
}

ChatMessage make_tool_result(const std::string& call_id,
                             const std::string& body)
{
    ChatMessage m;
    m.role         = MessageRole::tool;
    m.tool_call_id = call_id;
    m.content      = body;
    return m;
}

struct RecordingFrontend : IFrontend {
    std::vector<ActivityEvent> appended;
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
    void on_activity_updated(const ActivityEvent&) override {}
};

} // namespace

TEST_CASE("session restore: every ChatMessage field round-trips",
          "[session-restore][round-trip]")
{
    auto dir = make_temp_dir("history");
    SessionManager mgr(dir);

    ConversationHistory h;
    h.add(ChatMessage{MessageRole::system, "system prompt body"});
    h.add(make_user("show me the auth flow"));
    h.add(make_assistant_with_thinking(
        "I'll start by listing src/auth.",
        "Need to figure out where the entry point is. "
        "Guessing it's src/auth/ but should verify with list_directory."));
    h.add(make_assistant_with_tool_call("call_123", "list_directory",
                                        R"({"path":"src/auth","depth":1})"));
    h.add(make_tool_result("call_123", "auth.cpp\nauth.h\n"));
    h.add(ChatMessage{MessageRole::assistant,
                      "Two files; auth.cpp implements `authenticate()`."});

    auto id = mgr.save(h);

    auto loaded = mgr.load(id);
    REQUIRE(loaded.size() == h.size());

    const auto& orig    = h.messages();
    const auto& restored = loaded.messages();
    for (size_t i = 0; i < orig.size(); ++i) {
        INFO("message index " << i);
        REQUIRE(restored[i].role             == orig[i].role);
        REQUIRE(restored[i].content          == orig[i].content);
        REQUIRE(restored[i].reasoning_content == orig[i].reasoning_content);
        REQUIRE(restored[i].tool_call_id     == orig[i].tool_call_id);
        REQUIRE(restored[i].tool_calls.size() == orig[i].tool_calls.size());
        for (size_t k = 0; k < orig[i].tool_calls.size(); ++k) {
            REQUIRE(restored[i].tool_calls[k].id        == orig[i].tool_calls[k].id);
            REQUIRE(restored[i].tool_calls[k].name      == orig[i].tool_calls[k].name);
            REQUIRE(restored[i].tool_calls[k].arguments == orig[i].tool_calls[k].arguments);
        }
    }

    fs::remove_all(dir);
}

TEST_CASE("session restore: plan_to_json / plan_from_json round-trip",
          "[session-restore][plan]")
{
    Plan p;
    p.id      = "plan-7";
    p.title   = "Rewrite auth";
    p.summary = "Three steps, parallel-safe";

    PlanStep s1;
    s1.description  = "Inventory current auth code";
    s1.tools_needed = {"list_directory", "read_file"};
    s1.status       = PlanStep::Status::done;
    s1.notes        = "Found 4 files under src/auth/";
    p.steps.push_back(s1);

    PlanStep s2;
    s2.description  = "Replace token store";
    s2.tools_needed = {"edit_file"};
    s2.status       = PlanStep::Status::in_progress;
    p.steps.push_back(s2);

    PlanStep s3;
    s3.description  = "Update integration tests";
    s3.status       = PlanStep::Status::pending;
    p.steps.push_back(s3);

    auto j = plan_to_json(p);
    auto back = plan_from_json(j);

    REQUIRE(back.id      == p.id);
    REQUIRE(back.title   == p.title);
    REQUIRE(back.summary == p.summary);
    REQUIRE(back.steps.size() == p.steps.size());
    for (size_t i = 0; i < p.steps.size(); ++i) {
        INFO("step " << i);
        REQUIRE(back.steps[i].description  == p.steps[i].description);
        REQUIRE(back.steps[i].tools_needed == p.steps[i].tools_needed);
        REQUIRE(back.steps[i].status       == p.steps[i].status);
        REQUIRE(back.steps[i].notes        == p.steps[i].notes);
    }
}

TEST_CASE("session restore: SessionManager carries top-level extras "
          "(agent_mode + plan) through save / load_full",
          "[session-restore][extras]")
{
    auto dir = make_temp_dir("extras");
    SessionManager mgr(dir);

    ConversationHistory h;
    h.add(ChatMessage{MessageRole::system, "sys"});
    h.add(make_user("hi"));

    Plan p;
    p.id = "plan-1";
    p.title = "Demo";
    p.steps.push_back(PlanStep{"step one", {}, PlanStep::Status::pending, ""});

    nlohmann::json extras;
    extras["agent_mode"] = "plan";
    extras["plan"]       = plan_to_json(p);
    extras["plan_awaiting_decision"] = true;

    auto id = mgr.save(h, extras);

    auto full = mgr.load_full(id);
    REQUIRE(full.contains("messages"));
    REQUIRE(full.value("agent_mode", std::string{}) == "plan");
    REQUIRE(full.value("plan_awaiting_decision", false) == true);
    REQUIRE(full.contains("plan"));

    Plan back = plan_from_json(full["plan"]);
    REQUIRE(back.id == "plan-1");
    REQUIRE(back.title == "Demo");
    REQUIRE(back.steps.size() == 1);
    REQUIRE(back.steps[0].description == "step one");

    fs::remove_all(dir);
}

TEST_CASE("activity sidecar: emit appends JSONL, replay rebuilds the ring",
          "[session-restore][activity]")
{
    auto dir = make_temp_dir("activity");
    auto sidecar = dir / "demo.activity.jsonl";

    FrontendRegistry reg_a;
    ActivityLog log_a(reg_a);
    log_a.set_persist_path(sidecar);

    log_a.emit(ActivityKind::user_message, "user: hi");
    log_a.emit(ActivityKind::tool_call,    "tool_call: read_file",
               R"({"path":"src/a.cpp"})");
    log_a.emit(ActivityKind::tool_result,  "tool_result: read_file",
               "...body...", std::nullopt, std::nullopt, 42);
    log_a.emit(ActivityKind::llm_response, "llm: done",
               "summary", 100, 200, 5);

    REQUIRE(fs::exists(sidecar));

    // Fresh log replays the file. Use a new registry so the broadcast on
    // replay lands in a known place.
    FrontendRegistry reg_b;
    RecordingFrontend fe;
    reg_b.register_frontend(&fe);
    ActivityLog log_b(reg_b);
    log_b.replay_from(sidecar);

    auto buf = log_b.get_since(0);
    REQUIRE(buf.size() == 4);
    REQUIRE(buf[0].kind    == ActivityKind::user_message);
    REQUIRE(buf[0].summary == "user: hi");
    REQUIRE(buf[1].kind    == ActivityKind::tool_call);
    REQUIRE(buf[1].detail  == R"({"path":"src/a.cpp"})");
    REQUIRE(buf[2].tokens_delta.value() == 42);
    REQUIRE(buf[3].tokens_in.value()    == 100);
    REQUIRE(buf[3].tokens_out.value()   == 200);
    REQUIRE(buf[3].tokens_delta.value() == 5);

    // The replay re-broadcasts every event so a freshly-attached frontend
    // can paint them. We attached `fe` BEFORE replay_from, so it sees them
    // all.
    REQUIRE(fe.appended.size() == 4);

    fs::remove_all(dir);
}

TEST_CASE("activity sidecar: coalesced index_events flatten on replay",
          "[session-restore][activity][coalesce]")
{
    auto dir = make_temp_dir("activity_coalesce");
    auto sidecar = dir / "demo.activity.jsonl";

    FrontendRegistry reg;
    ActivityLog log(reg);
    log.set_persist_path(sidecar);

    // ActivityLog coalesces consecutive index_event rows that share a
    // category prefix -- the sidecar gets one line per emit (the file is
    // append-only) but the replay last-wins dedupe should leave us with a
    // single entry showing the final state.
    log.emit_index_event("Indexed 1 file",  "+ a.cpp");
    log.emit_index_event("Indexed 2 files", "+ a.cpp\n+ b.cpp");
    log.emit_index_event("Indexed 3 files", "+ a.cpp\n+ b.cpp\n+ c.cpp");

    FrontendRegistry reg2;
    ActivityLog log2(reg2);
    log2.replay_from(sidecar);

    auto buf = log2.get_since(0);
    REQUIRE(buf.size() == 1);
    REQUIRE(buf[0].summary == "Indexed 3 files");
    REQUIRE(buf[0].detail.find("+ c.cpp") != std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("session restore: SessionManager::remove also drops the activity sidecar",
          "[session-restore][activity][cleanup]")
{
    auto dir = make_temp_dir("activity_cleanup");
    SessionManager mgr(dir);

    ConversationHistory h;
    h.add(ChatMessage{MessageRole::system, "sys"});
    h.add(make_user("hi"));
    auto id = mgr.save(h);

    // Manually create the sidecar so we don't need a live AgentCore.
    auto sidecar = dir / (id + ".activity.jsonl");
    {
        std::ofstream f(sidecar);
        f << R"({"id":1,"ts":1716000000,"kind":"user_message","summary":"hi"})" << '\n';
    }
    REQUIRE(fs::exists(sidecar));

    REQUIRE(mgr.remove(id));
    REQUIRE_FALSE(fs::exists(sidecar));
    REQUIRE_FALSE(fs::exists(dir / (id + ".json")));

    fs::remove_all(dir);
}

TEST_CASE("session restore: load_full throws on missing file",
          "[session-restore][error]")
{
    auto dir = make_temp_dir("missing");
    SessionManager mgr(dir);
    REQUIRE_THROWS(mgr.load_full("does-not-exist"));
    fs::remove_all(dir);
}
