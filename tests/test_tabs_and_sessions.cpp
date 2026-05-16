// S5.I -- Multi-tab conversations + Session menu redesign.
//
// Lightweight unit tests that exercise the load-bearing pieces without
// requiring a live LLM or a fully-constructed Workspace. Most tests target
// SessionManager (cleanup criteria, last_opened_at bookkeeping, archive
// directory delete) which is hermetic. The LocusTab / LocusSession
// lifecycle is sketched at the SessionManager level -- the integration
// (real LLMConfig, real AgentCore) is exercised by the locus_integration
// suite, not here.

#include <catch2/catch_test_macros.hpp>

#include "agent/session_manager.h"
#include "core/workspace_ui_state.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>
#include <chrono>

using namespace locus;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path make_temp_sessions_dir(const std::string& tag)
{
    auto base = fs::temp_directory_path() / ("locus_s5i_" + tag + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base);
    return base;
}

// Helper: write a fake session file directly with controlled `last_opened_at`.
// Used so we can stage "old sessions" without waiting real wall time.
std::string write_fake_session(const fs::path& dir,
                                const std::string& id,
                                long long last_opened_at,
                                int message_count = 1,
                                const std::string& title = "")
{
    json j;
    j["id"] = id;
    j["message_count"] = message_count;
    j["estimated_tokens"] = 100;
    j["messages"] = json::array();
    j["first_user_message"] = title.empty() ? "test message" : title;
    j["title"] = title;
    j["created_at"]     = last_opened_at;
    j["last_opened_at"] = last_opened_at;

    std::ofstream f(dir / (id + ".json"));
    f << j.dump(2);
    return id;
}

} // namespace

// ---------------------------------------------------------------------------
// SessionManager: last_opened_at, created_at, title persistence
// ---------------------------------------------------------------------------

TEST_CASE("SessionManager stamps created_at and last_opened_at on first save",
          "[s5.i][session-manager][persistence]")
{
    auto dir = make_temp_sessions_dir("created_at");
    SessionManager sm(dir);

    ConversationHistory h;
    ChatMessage m;
    m.role = MessageRole::user;
    m.content = "hello";
    h.add(m);

    long long before = now_epoch_seconds();
    auto id = sm.save(h);
    long long after = now_epoch_seconds();

    auto list = sm.list();
    REQUIRE(list.size() == 1);
    auto& info = list.front();
    REQUIRE(info.id == id);
    REQUIRE(info.created_at >= before);
    REQUIRE(info.created_at <= after);
    REQUIRE(info.last_opened_at >= before);
    REQUIRE(info.last_opened_at <= after);
    // Title autoderived from the first user message.
    REQUIRE(info.title == "hello");
}

TEST_CASE("SessionManager::bump_last_opened_at bumps without touching created_at",
          "[s5.i][session-manager][last-opened]")
{
    auto dir = make_temp_sessions_dir("bump");
    SessionManager sm(dir);

    // Stage a session with a stale last_opened_at and a known created_at.
    long long ten_days_ago = now_epoch_seconds() - 10LL * 86400;
    write_fake_session(dir, "2024-01-01_000000_aaaa", ten_days_ago);

    auto before = sm.list();
    REQUIRE(before.size() == 1);
    long long created_before = before.front().created_at;

    REQUIRE(sm.bump_last_opened_at("2024-01-01_000000_aaaa"));

    auto after = sm.list();
    REQUIRE(after.size() == 1);
    REQUIRE(after.front().last_opened_at > before.front().last_opened_at);
    // created_at unchanged.
    REQUIRE(after.front().created_at == created_before);
}

TEST_CASE("SessionManager::rename persists the new title",
          "[s5.i][session-manager][rename]")
{
    auto dir = make_temp_sessions_dir("rename");
    SessionManager sm(dir);

    write_fake_session(dir, "id_1", now_epoch_seconds(), 1, "original");

    REQUIRE(sm.rename("id_1", "renamed"));
    auto list = sm.list();
    REQUIRE(list.size() == 1);
    REQUIRE(list.front().title == "renamed");
}

// ---------------------------------------------------------------------------
// SessionManager::cleanup_candidates
// ---------------------------------------------------------------------------

TEST_CASE("cleanup_candidates respects keep_last_count",
          "[s5.i][session-manager][cleanup]")
{
    auto dir = make_temp_sessions_dir("keep_last");
    SessionManager sm(dir);

    long long now = now_epoch_seconds();
    long long old = now - 100LL * 86400;  // 100 days ago

    // 5 old sessions, all old enough -- but keep_last_count=10 protects all.
    for (int i = 0; i < 5; ++i)
        write_fake_session(dir, "old_" + std::to_string(i),
                            old + i, 1, "old " + std::to_string(i));

    auto candidates = sm.cleanup_candidates(/*keep_last=*/10,
                                            /*delete_after_days=*/30, {});
    REQUIRE(candidates.empty());
}

TEST_CASE("cleanup_candidates respects delete_after_days",
          "[s5.i][session-manager][cleanup]")
{
    auto dir = make_temp_sessions_dir("after_days");
    SessionManager sm(dir);

    long long now = now_epoch_seconds();
    // 3 recent sessions (1 day ago) + 5 old sessions (60 days ago).
    for (int i = 0; i < 3; ++i)
        write_fake_session(dir, "recent_" + std::to_string(i),
                            now - 86400 + i, 1, "recent " + std::to_string(i));
    for (int i = 0; i < 5; ++i)
        write_fake_session(dir, "old_" + std::to_string(i),
                            now - 60LL * 86400 + i, 1, "old " + std::to_string(i));

    // keep_last=3 protects the 3 recent. delete_after_days=30 flags only the
    // 5 old ones.
    auto candidates = sm.cleanup_candidates(/*keep_last=*/3,
                                            /*delete_after_days=*/30, {});
    REQUIRE(candidates.size() == 5);
}

TEST_CASE("cleanup_candidates exempts currently-open sessions",
          "[s5.i][session-manager][cleanup]")
{
    auto dir = make_temp_sessions_dir("open_exempt");
    SessionManager sm(dir);

    long long old = now_epoch_seconds() - 60LL * 86400;
    write_fake_session(dir, "open_one", old + 1, 1, "open");
    write_fake_session(dir, "closed_one", old + 2, 1, "closed");

    std::unordered_set<std::string> open = {"open_one"};
    auto candidates = sm.cleanup_candidates(/*keep_last=*/0,
                                            /*delete_after_days=*/30, open);
    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates.front() == "closed_one");
}

// ---------------------------------------------------------------------------
// SessionManager::remove also removes the archive directory
// ---------------------------------------------------------------------------

TEST_CASE("remove() deletes both the JSON file and the archive directory",
          "[s5.i][session-manager][remove][archive]")
{
    auto dir = make_temp_sessions_dir("archive");
    SessionManager sm(dir);

    long long now = now_epoch_seconds();
    write_fake_session(dir, "with_archive", now);

    // Synthesize an archive subdirectory with two pre-compaction snapshots.
    auto archive_dir = dir / "with_archive";
    fs::create_directories(archive_dir);
    std::ofstream(archive_dir / "history.before-compact-1.json") << "{}";
    std::ofstream(archive_dir / "history.before-compact-2.json") << "{}";

    REQUIRE(fs::exists(dir / "with_archive.json"));
    REQUIRE(fs::exists(archive_dir));

    REQUIRE(sm.remove("with_archive"));

    REQUIRE_FALSE(fs::exists(dir / "with_archive.json"));
    REQUIRE_FALSE(fs::exists(archive_dir));
}

TEST_CASE("size_bytes in SessionInfo sums file + archive directory",
          "[s5.i][session-manager][archive][size]")
{
    auto dir = make_temp_sessions_dir("size");
    SessionManager sm(dir);

    long long now = now_epoch_seconds();
    write_fake_session(dir, "size_test", now);

    auto archive_dir = dir / "size_test";
    fs::create_directories(archive_dir);
    {
        std::ofstream f(archive_dir / "history.before-compact-1.json");
        for (int i = 0; i < 1000; ++i) f << "x";
    }

    auto list = sm.list();
    REQUIRE(list.size() == 1);
    // The base JSON has some size + we wrote 1000 bytes into the archive.
    REQUIRE(list.front().size_bytes >= 1000);
}

// ---------------------------------------------------------------------------
// Legacy compatibility: old session files without title / last_opened_at
// ---------------------------------------------------------------------------

TEST_CASE("Legacy session file loads with derived title and mtime fallback",
          "[s5.i][session-manager][legacy]")
{
    auto dir = make_temp_sessions_dir("legacy");
    SessionManager sm(dir);

    // Pre-S5.I shape: no `title`, no `created_at`, no `last_opened_at`.
    json j;
    j["id"] = "legacy_1";
    j["message_count"] = 2;
    j["estimated_tokens"] = 100;
    j["messages"] = json::array();
    j["first_user_message"] = "Refactor the parser";
    std::ofstream(dir / "legacy_1.json") << j.dump(2);

    auto list = sm.list();
    REQUIRE(list.size() == 1);
    auto& info = list.front();
    // Title falls back to first_user_message preview.
    REQUIRE(info.title == "Refactor the parser");
    // last_opened_at + created_at default to file mtime (non-zero).
    REQUIRE(info.last_opened_at > 0);
    REQUIRE(info.created_at > 0);
}

// ---------------------------------------------------------------------------
// WorkspaceUiState round-trip
// ---------------------------------------------------------------------------

TEST_CASE("WorkspaceUiState round-trips open_tabs", "[s5.i][workspace-ui-state]")
{
    auto dir = make_temp_sessions_dir("uistate");
    auto locus_dir = dir;

    WorkspaceUiState w;
    WorkspaceUiState::OpenTab a, b;
    a.session_id = "id_a"; a.title = "Research thread"; a.active = false;
    b.session_id = "id_b"; b.title = "Implementation"; b.active = true;
    w.open_tabs.push_back(a);
    w.open_tabs.push_back(b);

    REQUIRE(save_workspace_ui_state(locus_dir, w));

    auto loaded = load_workspace_ui_state(locus_dir);
    REQUIRE(loaded.open_tabs.size() == 2);
    REQUIRE(loaded.open_tabs[0].session_id == "id_a");
    REQUIRE(loaded.open_tabs[0].title == "Research thread");
    REQUIRE_FALSE(loaded.open_tabs[0].active);
    REQUIRE(loaded.open_tabs[1].session_id == "id_b");
    REQUIRE(loaded.open_tabs[1].active);
}

TEST_CASE("WorkspaceUiState ignores empty session_ids on save",
          "[s5.i][workspace-ui-state]")
{
    auto dir = make_temp_sessions_dir("uistate_empty");
    auto locus_dir = dir;

    WorkspaceUiState w;
    WorkspaceUiState::OpenTab a;
    a.session_id = "id_a"; a.title = "One"; a.active = true;
    w.open_tabs.push_back(a);
    WorkspaceUiState::OpenTab empty;
    empty.session_id = ""; empty.title = "Empty"; empty.active = false;
    w.open_tabs.push_back(empty);

    REQUIRE(save_workspace_ui_state(locus_dir, w));

    auto loaded = load_workspace_ui_state(locus_dir);
    // Only the one with session_id survives.
    REQUIRE(loaded.open_tabs.size() == 1);
    REQUIRE(loaded.open_tabs[0].session_id == "id_a");
}

// ---------------------------------------------------------------------------
// ID generation is collision-free across concurrent saves
// ---------------------------------------------------------------------------

TEST_CASE("Concurrent saves produce unique session ids",
          "[s5.i][session-manager][id-collision]")
{
    auto dir = make_temp_sessions_dir("collide");
    SessionManager sm(dir);

    // Two tabs typing first message within the same second -- the random
    // suffix in generate_id() prevents collision.
    constexpr int N = 16;
    std::vector<std::string> ids(N);
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i] {
            ConversationHistory h;
            ChatMessage m;
            m.role = MessageRole::user;
            m.content = "tab " + std::to_string(i);
            h.add(m);
            ids[i] = sm.save(h);
        });
    }
    for (auto& t : threads) t.join();

    std::unordered_set<std::string> uniq(ids.begin(), ids.end());
    REQUIRE(uniq.size() == ids.size());
}
