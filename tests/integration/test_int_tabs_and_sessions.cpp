// S5.I -- Multi-tab + session lifecycle integration test.
//
// Spins up a self-contained LocusSession on a temp workspace, drives two
// tabs through a real LLM turn each, and verifies:
//
//   1. Tab A and Tab B histories stay isolated (the agent in tab B does NOT
//      see tab A's user message).
//   2. Lazy session-file creation: an untouched tab leaves no JSON on disk;
//      a tab that has handled one user message has a corresponding `.json`
//      under `.locus/sessions/`.
//   3. Auto-save title derivation: the saved JSON's `title` is the
//      first user message preview (truncated to 30 chars).
//   4. Close on a non-empty tab persists the session file; close on an
//      empty tab discards it silently. Closing the last tab opens a fresh
//      empty one.
//   5. Saved-session backward compatibility: a pre-S5.I JSON (no title,
//      no last_opened_at) loads into a tab with auto-derived title and a
//      non-zero last_opened_at.
//
// We deliberately do NOT extend the shared IntegrationHarness with a
// multi-tab shape: the harness opens WS1 and adding tabs to it would mix
// dev state into test runs. A local LocusSession owns its own workspace +
// SessionManager and tears them down at scope end.

#include "harness_fixture.h"

#include "agent/conversation.h"
#include "core/locus_session.h"
#include "core/locus_tab.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

using namespace locus;
using namespace locus::integration;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

// Lightweight tmp workspace -- no MCP, no semantic search, no embedder. Keeps
// the cold-start cost down so each test case lights up in under a second
// before the LLM turn dominates the budget.
fs::path make_temp_workspace(const std::string& tag)
{
    auto unique = tag + "_" + std::to_string(std::random_device{}());
    auto root = fs::temp_directory_path() / ("locus_int_s5i_" + unique);
    fs::create_directories(root / ".locus");

    std::ofstream f(root / ".locus" / "config.json");
    f << R"({
  "index": { "semantic_search": { "enabled": false } },
  "memory": { "enabled": false },
  "capabilities": {
    "background_processes": false,
    "semantic_search": false,
    "code_aware_search": false,
    "memory_bank": false,
    "web_retrieval": false
  }
})";
    return root;
}

void best_effort_remove(const fs::path& p)
{
    for (int i = 0; i < 20; ++i) {
        std::error_code ec;
        fs::remove_all(p, ec);
        if (!ec) return;
        std::this_thread::sleep_for(50ms);
    }
}

LLMConfig make_llm_config()
{
    LLMConfig cfg;
    if (const char* ep = std::getenv("LOCUS_INT_TEST_ENDPOINT"))
        cfg.base_url = ep;
    if (const char* mid = std::getenv("LOCUS_INT_TEST_MODEL"))
        cfg.model = mid;
    cfg.temperature = 0.2;
    cfg.max_tokens  = 1024;
    cfg.timeout_ms  = 120000;
    return cfg;
}

// Helper: drive a tab's agent through one user-message turn end-to-end.
// Returns true on completion within the timeout, false on time-out. Uses a
// fresh HarnessFrontend so capture state is scoped to this call.
bool prompt_tab(LocusTab& tab, const std::string& msg,
                std::chrono::milliseconds timeout = 3min)
{
    HarnessFrontend fe;
    fe.attach_core(&tab.agent());
    tab.agent().register_frontend(&fe);
    fe.reset_turn_state();
    tab.agent().send_message(msg);
    bool ok = fe.wait_for_turn(timeout);
    tab.agent().unregister_frontend(&fe);
    return ok;
}

} // namespace

#ifdef _WIN32

TEST_CASE("Multi-tab isolation: agent B does not see agent A's history",
          "[integration][llm][tabs]")
{
    // Verify LM Studio is reachable via the shared harness, then build our
    // own short-lived session for this test.
    auto& h = harness();
    (void)h;  // touch so the LM Studio reachability error fires early.

    auto root = make_temp_workspace("isolation");
    struct Cleanup {
        fs::path p;
        ~Cleanup() { best_effort_remove(p); }
    } cleanup{root};

    LocusSession session(root, make_llm_config());

    int a = session.add_tab();
    int b = session.add_tab();
    REQUIRE(a == 0);
    REQUIRE(b == 1);
    REQUIRE(session.tab_count() == 2);

    auto& tab_a = session.tab(a);
    auto& tab_b = session.tab(b);

    // Each tab has its own AgentCore -- distinct objects.
    REQUIRE(&tab_a.agent() != &tab_b.agent());

    // Send one short message to tab A only.
    REQUIRE(prompt_tab(tab_a,
        "Reply with the single word: alpha."));

    // Tab B's history should still be just the system prompt -- no user
    // messages, no assistant responses.
    bool b_has_user = false;
    for (const auto& m : tab_b.agent().history().messages()) {
        if (m.role == MessageRole::user) b_has_user = true;
    }
    REQUIRE_FALSE(b_has_user);

    // Tab A's history must include the user message we just sent.
    bool a_has_user = false;
    for (const auto& m : tab_a.agent().history().messages()) {
        if (m.role == MessageRole::user && m.content.find("alpha") != std::string::npos)
            a_has_user = true;
    }
    REQUIRE(a_has_user);
}

TEST_CASE("Lazy session-file creation: empty tab leaves no JSON on disk",
          "[integration][tabs][persistence]")
{
    auto root = make_temp_workspace("lazy");
    struct Cleanup { fs::path p; ~Cleanup() { best_effort_remove(p); } } cleanup{root};

    auto sessions_dir = root / ".locus" / "sessions";

    {
        LocusSession session(root, make_llm_config());
        session.add_tab();
        REQUIRE(session.tab_count() == 1);
        // No user message sent -- the tab has session_id == empty.
        REQUIRE(session.tab(0).session_id().empty());
    }

    // After the session is destroyed, sessions_dir is either missing or has
    // no JSON files in it. The directory may have been created by
    // SessionManager's ctor; we just need to know no JSON landed.
    bool any_json = false;
    if (fs::exists(sessions_dir)) {
        for (auto& e : fs::directory_iterator(sessions_dir)) {
            if (e.is_regular_file() && e.path().extension() == ".json")
                any_json = true;
        }
    }
    REQUIRE_FALSE(any_json);
}

TEST_CASE("Auto-save fires after first user message; title autoderived",
          "[integration][llm][tabs][persistence]")
{
    auto& h = harness();
    (void)h;

    auto root = make_temp_workspace("autosave");
    struct Cleanup { fs::path p; ~Cleanup() { best_effort_remove(p); } } cleanup{root};

    {
        LocusSession session(root, make_llm_config());
        session.set_active_tab(session.add_tab());
        auto& tab = session.active_tab();

        REQUIRE(prompt_tab(tab,
            "Please reply with: hi from S5.I tab."));

        // Session id is assigned lazily on first user message; auto-save
        // has stamped a JSON to disk.
        REQUIRE_FALSE(tab.session_id().empty());

        auto path = root / ".locus" / "sessions" / (tab.session_id() + ".json");
        REQUIRE(fs::exists(path));

        std::ifstream f(path);
        nlohmann::json j = nlohmann::json::parse(f);
        REQUIRE(j.contains("title"));
        const std::string title = j["title"].get<std::string>();
        REQUIRE_FALSE(title.empty());
        // Title is auto-derived from the first user message, truncated to
        // 30 chars; first user message starts with "Please reply...".
        REQUIRE(title.find("Please reply") != std::string::npos);
        REQUIRE(title.size() <= 30);
        REQUIRE(j.contains("created_at"));
        REQUIRE(j.contains("last_opened_at"));
        REQUIRE(j["created_at"].get<long long>() > 0);
        REQUIRE(j["last_opened_at"].get<long long>() > 0);
    }
}

TEST_CASE("close_tab persists a non-empty tab and discards an empty one",
          "[integration][llm][tabs][close]")
{
    auto& h = harness();
    (void)h;

    auto root = make_temp_workspace("close");
    struct Cleanup { fs::path p; ~Cleanup() { best_effort_remove(p); } } cleanup{root};

    LocusSession session(root, make_llm_config());
    session.add_tab();          // tab 0: empty
    session.add_tab();          // tab 1: will be non-empty
    REQUIRE(session.tab_count() == 2);

    // Send a message to tab 1 so it becomes non-empty.
    REQUIRE(prompt_tab(session.tab(1),
        "One-word reply: ok."));
    REQUIRE_FALSE(session.tab(1).session_id().empty());
    std::string saved_id = session.tab(1).session_id();

    // Close tab 0 (the empty one). It's index 0 currently.
    session.close_tab(0);
    REQUIRE(session.tab_count() == 1);

    // The remaining tab is the previously-non-empty one. Its session id
    // matches what got saved earlier.
    REQUIRE(session.tab(0).session_id() == saved_id);

    // Now close the last tab (non-empty). The session opens a fresh empty
    // tab in its place -- workspace is never tab-less.
    session.close_tab(0);
    REQUIRE(session.tab_count() == 1);
    REQUIRE(session.tab(0).session_id().empty());

    // The saved JSON for the non-empty tab is still on disk.
    auto saved_path = root / ".locus" / "sessions" / (saved_id + ".json");
    REQUIRE(fs::exists(saved_path));
}

TEST_CASE("Saved-session backward compatibility: legacy JSON loads with derived title",
          "[integration][tabs][legacy]")
{
    auto root = make_temp_workspace("legacy");
    struct Cleanup { fs::path p; ~Cleanup() { best_effort_remove(p); } } cleanup{root};

    auto sessions_dir = root / ".locus" / "sessions";
    fs::create_directories(sessions_dir);

    // Pre-S5.I shape: no `title`, no `created_at`, no `last_opened_at`.
    nlohmann::json j;
    j["id"] = "legacy_session_1";
    j["message_count"] = 2;
    j["estimated_tokens"] = 100;
    j["first_user_message"] = "Refactor the parser";
    j["messages"] = nlohmann::json::array();
    j["messages"].push_back({
        {"role", "system"},
        {"content", "You are a test assistant."}
    });
    j["messages"].push_back({
        {"role", "user"},
        {"content", "Refactor the parser"}
    });
    {
        std::ofstream f(sessions_dir / "legacy_session_1.json");
        f << j.dump(2);
    }

    LocusSession session(root, make_llm_config());
    int idx = session.add_tab_for_session("legacy_session_1");
    REQUIRE(idx == 0);

    auto& tab = session.tab(idx);
    REQUIRE(tab.session_id() == "legacy_session_1");
    // Title derived from the first user message preview.
    REQUIRE(tab.title().find("Refactor the parser") != std::string::npos);

    // SessionInfo for the loaded session now has a non-zero last_opened_at
    // (bumped by load_session) and a derived title.
    auto list = session.sessions().list();
    bool found = false;
    for (const auto& info : list) {
        if (info.id == "legacy_session_1") {
            found = true;
            REQUIRE(info.last_opened_at > 0);
            REQUIRE(info.created_at > 0);
            REQUIRE_FALSE(info.title.empty());
        }
    }
    REQUIRE(found);
}

TEST_CASE("delete_tab removes both the JSON file and the archive subdirectory",
          "[integration][llm][tabs][delete]")
{
    auto& h = harness();
    (void)h;

    auto root = make_temp_workspace("delete");
    struct Cleanup { fs::path p; ~Cleanup() { best_effort_remove(p); } } cleanup{root};

    LocusSession session(root, make_llm_config());
    session.set_active_tab(session.add_tab());

    REQUIRE(prompt_tab(session.active_tab(),
        "Reply with single word: done."));

    std::string sid = session.active_tab().session_id();
    REQUIRE_FALSE(sid.empty());

    auto sessions_dir = root / ".locus" / "sessions";
    auto json_path = sessions_dir / (sid + ".json");
    auto archive_dir = sessions_dir / sid;

    // Synthesise an archive directory (would normally be created by S5.F
    // compaction) so we can verify delete_tab wipes it.
    fs::create_directories(archive_dir);
    {
        std::ofstream f(archive_dir / "history.before-compact-1.json");
        f << "{}";
    }
    REQUIRE(fs::exists(json_path));
    REQUIRE(fs::exists(archive_dir));

    session.delete_tab(0);

    // Both the file and the directory must be gone.
    REQUIRE_FALSE(fs::exists(json_path));
    REQUIRE_FALSE(fs::exists(archive_dir));
    // Workspace is never tab-less; a fresh empty tab opens in place.
    REQUIRE(session.tab_count() == 1);
    REQUIRE(session.tab(0).session_id().empty());
}

#endif  // _WIN32
