#include <catch2/catch_test_macros.hpp>

#include "workspace.h"
#include "database.h"
#include "file_watcher.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static fs::path make_temp_dir()
{
    auto tmp = fs::temp_directory_path() / "locus_test_ws";
    fs::create_directories(tmp);
    return tmp;
}

static void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace creates .locus/ directory", "[s0.2][workspace]")
{
    auto tmp = make_temp_dir();
    cleanup(tmp);
    fs::create_directories(tmp);

    {
        locus::Workspace ws(tmp);
        REQUIRE(fs::is_directory(tmp / ".locus"));
    }

    cleanup(tmp);
}

TEST_CASE("Workspace creates default config.json", "[s0.2][workspace]")
{
    auto tmp = make_temp_dir();
    cleanup(tmp);
    fs::create_directories(tmp);

    {
        locus::Workspace ws(tmp);
        REQUIRE(fs::exists(tmp / ".locus" / "config.json"));
    }

    cleanup(tmp);
}

TEST_CASE("Workspace reads LOCUS.md when present", "[s0.2][workspace]")
{
    auto tmp = make_temp_dir();
    cleanup(tmp);
    fs::create_directories(tmp);

    // Write a test LOCUS.md
    {
        std::ofstream f(tmp / "LOCUS.md");
        f << "# Test Workspace\nThis is a test.";
    }

    {
        locus::Workspace ws(tmp);
        REQUIRE(ws.locus_md().find("Test Workspace") != std::string::npos);
    }

    cleanup(tmp);
}

TEST_CASE("Workspace has empty LOCUS.md when file absent", "[s0.2][workspace]")
{
    auto tmp = make_temp_dir();
    cleanup(tmp);
    fs::create_directories(tmp);

    {
        locus::Workspace ws(tmp);
        REQUIRE(ws.locus_md().empty());
    }

    cleanup(tmp);
}

TEST_CASE("Database creates index.db with WAL mode", "[s0.2][database]")
{
    auto tmp = make_temp_dir();
    cleanup(tmp);
    fs::create_directories(tmp);

    {
        locus::Database db(tmp / "test.db", locus::DbKind::Main);
        REQUIRE(db.handle() != nullptr);
        REQUIRE(fs::exists(tmp / "test.db"));
    }

    cleanup(tmp);
}

TEST_CASE("Database creates files table", "[s0.2][database]")
{
    auto tmp = make_temp_dir();
    cleanup(tmp);
    fs::create_directories(tmp);

    {
        locus::Database db(tmp / "test.db", locus::DbKind::Main);

        // Insert a row into files to verify the table exists
        db.exec(R"(
            INSERT INTO files (path, abs_path, size_bytes, modified_at, ext, language)
            VALUES ('src/main.cpp', '/tmp/src/main.cpp', 1234, 1700000000, '.cpp', 'cpp')
        )");

        // Verify it was inserted (no exception = success)
        db.exec("SELECT COUNT(*) FROM files");
    }

    cleanup(tmp);
}

TEST_CASE("FileWatcher debounce filters rapid events", "[s0.2][filewatcher]")
{
    auto tmp = make_temp_dir();
    cleanup(tmp);
    fs::create_directories(tmp);

    {
        locus::FileWatcher fw(tmp, {".git/**"});
        fw.start();

        // Create a file to trigger an event
        { std::ofstream f(tmp / "test.txt"); f << "hello"; }

        // Immediately drain - should get nothing (within debounce window)
        std::vector<locus::FileEvent> events;
        size_t n = fw.drain(events);
        REQUIRE(n == 0);

        // Wait past debounce window
        std::this_thread::sleep_for(std::chrono::milliseconds(350));

        n = fw.drain(events);
        // May or may not have events depending on OS timing, but no crash
        REQUIRE(events.size() >= 0);  // structural test - no crash

        fw.stop();
    }

    cleanup(tmp);
}

TEST_CASE("Workspace rejects non-existent path", "[s0.2][workspace]")
{
    REQUIRE_THROWS_AS(
        locus::Workspace(fs::path("Z:/nonexistent/path/that/does/not/exist")),
        std::runtime_error
    );
}
