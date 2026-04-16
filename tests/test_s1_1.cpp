#include <catch2/catch_test_macros.hpp>

#include "frontend.h"
#include "frontend_registry.h"
#include "session_manager.h"
#include "conversation.h"
#include "llm_client.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace locus;
namespace fs = std::filesystem;

// ---- Stub frontend for testing ----------------------------------------------

struct StubFrontend : IFrontend {
    std::vector<std::string> events;

    void on_turn_start() override { events.push_back("turn_start"); }
    void on_token(std::string_view t) override { events.push_back("token:" + std::string(t)); }
    void on_tool_call_pending(const ToolCall&, const std::string&) override {
        events.push_back("tool_pending");
    }
    void on_tool_result(const std::string& id, const std::string&) override {
        events.push_back("tool_result:" + id);
    }
    void on_turn_complete() override { events.push_back("turn_complete"); }
    void on_context_meter(int used, int limit) override {
        events.push_back("ctx:" + std::to_string(used) + "/" + std::to_string(limit));
    }
    void on_compaction_needed(int, int) override { events.push_back("compaction"); }
    void on_session_reset() override { events.push_back("session_reset"); }
    void on_error(const std::string& msg) override { events.push_back("error:" + msg); }
    void on_embedding_progress(int, int) override {}
};

// Throws on every callback — used to test exception isolation.
struct ThrowingFrontend : IFrontend {
    void on_turn_start() override { throw std::runtime_error("boom"); }
    void on_token(std::string_view) override { throw std::runtime_error("boom"); }
    void on_tool_call_pending(const ToolCall&, const std::string&) override {
        throw std::runtime_error("boom");
    }
    void on_tool_result(const std::string&, const std::string&) override {
        throw std::runtime_error("boom");
    }
    void on_turn_complete() override { throw std::runtime_error("boom"); }
    void on_context_meter(int, int) override { throw std::runtime_error("boom"); }
    void on_compaction_needed(int, int) override { throw std::runtime_error("boom"); }
    void on_session_reset() override { throw std::runtime_error("boom"); }
    void on_error(const std::string&) override { throw std::runtime_error("boom"); }
    void on_embedding_progress(int, int) override { throw std::runtime_error("boom"); }
};

// ---- FrontendRegistry tests -------------------------------------------------

TEST_CASE("FrontendRegistry register and broadcast", "[s1.1]")
{
    FrontendRegistry reg;
    StubFrontend fe1, fe2;

    REQUIRE(reg.empty());

    reg.register_frontend(&fe1);
    reg.register_frontend(&fe2);

    REQUIRE_FALSE(reg.empty());

    reg.broadcast([](IFrontend& fe) { fe.on_turn_start(); });

    REQUIRE(fe1.events.size() == 1);
    REQUIRE(fe1.events[0] == "turn_start");
    REQUIRE(fe2.events.size() == 1);
    REQUIRE(fe2.events[0] == "turn_start");
}

TEST_CASE("FrontendRegistry unregister", "[s1.1]")
{
    FrontendRegistry reg;
    StubFrontend fe1, fe2;

    reg.register_frontend(&fe1);
    reg.register_frontend(&fe2);
    reg.unregister_frontend(&fe1);

    reg.broadcast([](IFrontend& fe) { fe.on_turn_start(); });

    REQUIRE(fe1.events.empty());
    REQUIRE(fe2.events.size() == 1);
}

TEST_CASE("FrontendRegistry exception isolation", "[s1.1]")
{
    FrontendRegistry reg;
    ThrowingFrontend bad;
    StubFrontend good;

    // Bad frontend registered first — should not prevent good from receiving.
    reg.register_frontend(&bad);
    reg.register_frontend(&good);

    // Should not throw, and good should still receive the event.
    REQUIRE_NOTHROW(
        reg.broadcast([](IFrontend& fe) { fe.on_turn_start(); })
    );
    REQUIRE(good.events.size() == 1);
    REQUIRE(good.events[0] == "turn_start");
}

// ---- SessionManager tests ---------------------------------------------------

static fs::path make_temp_sessions_dir()
{
    auto dir = fs::temp_directory_path() / "locus_test_sessions";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

TEST_CASE("SessionManager save and load round-trip", "[s1.1]")
{
    auto dir = make_temp_sessions_dir();
    SessionManager mgr(dir);

    ConversationHistory h;
    h.add({MessageRole::system, "System prompt"});
    h.add({MessageRole::user, "Hello there"});
    h.add({MessageRole::assistant, "Hi!"});

    auto id = mgr.save(h);
    REQUIRE_FALSE(id.empty());

    // File should exist.
    REQUIRE(fs::exists(dir / (id + ".json")));

    // Load it back.
    auto loaded = mgr.load(id);
    REQUIRE(loaded.size() == 3);
    REQUIRE(loaded.messages()[0].role == MessageRole::system);
    REQUIRE(loaded.messages()[1].content == "Hello there");
    REQUIRE(loaded.messages()[2].content == "Hi!");

    fs::remove_all(dir);
}

TEST_CASE("SessionManager list returns sessions newest-first", "[s1.1]")
{
    auto dir = make_temp_sessions_dir();
    SessionManager mgr(dir);

    ConversationHistory h1;
    h1.add({MessageRole::system, "S"});
    h1.add({MessageRole::user, "First"});
    mgr.save("2026-04-10_100000", h1);

    ConversationHistory h2;
    h2.add({MessageRole::system, "S"});
    h2.add({MessageRole::user, "Second"});
    mgr.save("2026-04-12_120000", h2);

    auto sessions = mgr.list();
    REQUIRE(sessions.size() == 2);
    // Newest first.
    REQUIRE(sessions[0].id == "2026-04-12_120000");
    REQUIRE(sessions[1].id == "2026-04-10_100000");
    REQUIRE(sessions[0].first_user_message == "Second");
    REQUIRE(sessions[0].message_count == 2);

    fs::remove_all(dir);
}

TEST_CASE("SessionManager remove", "[s1.1]")
{
    auto dir = make_temp_sessions_dir();
    SessionManager mgr(dir);

    ConversationHistory h;
    h.add({MessageRole::system, "S"});
    auto id = mgr.save(h);

    REQUIRE(mgr.remove(id));
    REQUIRE_FALSE(fs::exists(dir / (id + ".json")));
    REQUIRE_FALSE(mgr.remove(id));  // already gone

    fs::remove_all(dir);
}

TEST_CASE("SessionManager load nonexistent throws", "[s1.1]")
{
    auto dir = make_temp_sessions_dir();
    SessionManager mgr(dir);

    REQUIRE_THROWS_AS(mgr.load("nonexistent"), std::runtime_error);

    fs::remove_all(dir);
}

TEST_CASE("SessionManager preserves tool calls in round-trip", "[s1.1]")
{
    auto dir = make_temp_sessions_dir();
    SessionManager mgr(dir);

    ConversationHistory h;
    h.add({MessageRole::system, "System"});
    h.add({MessageRole::user, "Read test.txt"});

    ChatMessage assistant_msg;
    assistant_msg.role = MessageRole::assistant;
    assistant_msg.content = "";
    assistant_msg.tool_calls = {{"call_1", "read_file", R"({"path":"test.txt"})"}};
    h.add(std::move(assistant_msg));

    ChatMessage tool_msg;
    tool_msg.role = MessageRole::tool;
    tool_msg.tool_call_id = "call_1";
    tool_msg.content = "File contents here.";
    h.add(std::move(tool_msg));

    auto id = mgr.save(h);
    auto loaded = mgr.load(id);

    REQUIRE(loaded.size() == 4);
    REQUIRE(loaded.messages()[2].tool_calls.size() == 1);
    REQUIRE(loaded.messages()[2].tool_calls[0].name == "read_file");
    REQUIRE(loaded.messages()[3].tool_call_id == "call_1");

    fs::remove_all(dir);
}
