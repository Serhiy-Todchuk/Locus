// S4.T -- file-change tracker between turns.
//
// Drives a real Workspace so the IndexQuery::list_directory the tracker leans
// on is exercised end-to-end. We bypass the asynchronous file watcher by
// feeding synthetic FileEvents into the indexer directly -- the tracker only
// cares about what the index DB reports, and the watcher pipeline already has
// its own coverage.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "agent/file_change_tracker.h"
#include "file_watcher.h"
#include "index/index_query.h"
#include "index/indexer.h"
#include "workspace.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

fs::path make_test_dir(const char* suffix)
{
    auto tmp = fs::temp_directory_path() / ("locus_test_fct_" + std::string(suffix));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    return tmp;
}

void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << content;
}

void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// FAT/NTFS file mtime resolution is at least 2 s on some filesystems; sleep
// long enough that a re-write is guaranteed to bump the mtime past the value
// we just snapshotted.
void mtime_gap()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
}

void notify_indexer(locus::Workspace& ws,
                    locus::FileAction action,
                    const std::string& rel)
{
    std::vector<locus::FileEvent> events{
        {action, fs::path(rel), {}}
    };
    ws.indexer().process_events(events);
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

using locus::FileAction;
using locus::FileChangeTracker;

TEST_CASE("FileChangeTracker: empty snapshot returns empty diff (first turn)",
          "[s4.t][file-change-tracker]")
{
    auto tmp = make_test_dir("empty");
    write_file(tmp / "a.txt", "hello");

    {
        locus::Workspace ws(tmp);
        FileChangeTracker tr;
        auto d = tr.diff_since_snapshot(ws.query());
        REQUIRE(d.changed.empty());
        REQUIRE(d.deleted.empty());
    }

    cleanup(tmp);
}

TEST_CASE("FileChangeTracker: snapshot then no change -> empty diff",
          "[s4.t][file-change-tracker]")
{
    auto tmp = make_test_dir("nochange");
    write_file(tmp / "a.txt", "hello");
    write_file(tmp / "b.txt", "world");

    {
        locus::Workspace ws(tmp);
        FileChangeTracker tr;
        tr.snapshot(ws.query());
        REQUIRE(tr.snapshot_size() >= 2);

        auto d = tr.diff_since_snapshot(ws.query());
        REQUIRE(d.changed.empty());
        REQUIRE(d.deleted.empty());
    }

    cleanup(tmp);
}

TEST_CASE("FileChangeTracker: detects modified file",
          "[s4.t][file-change-tracker]")
{
    auto tmp = make_test_dir("modified");
    write_file(tmp / "a.txt", "v1");
    write_file(tmp / "b.txt", "stable");

    {
        locus::Workspace ws(tmp);
        FileChangeTracker tr;
        tr.snapshot(ws.query());

        mtime_gap();
        write_file(tmp / "a.txt", "v2-modified-with-different-size");
        notify_indexer(ws, FileAction::Modified, "a.txt");

        auto d = tr.diff_since_snapshot(ws.query());
        REQUIRE(contains(d.changed, "a.txt"));
        REQUIRE_FALSE(contains(d.changed, "b.txt"));
        REQUIRE(d.deleted.empty());
    }

    cleanup(tmp);
}

TEST_CASE("FileChangeTracker: detects added file",
          "[s4.t][file-change-tracker]")
{
    auto tmp = make_test_dir("added");
    write_file(tmp / "a.txt", "exists");

    {
        locus::Workspace ws(tmp);
        FileChangeTracker tr;
        tr.snapshot(ws.query());

        write_file(tmp / "newcomer.txt", "fresh");
        notify_indexer(ws, FileAction::Added, "newcomer.txt");

        auto d = tr.diff_since_snapshot(ws.query());
        REQUIRE(contains(d.changed, "newcomer.txt"));
        REQUIRE(d.deleted.empty());
    }

    cleanup(tmp);
}

TEST_CASE("FileChangeTracker: detects deleted file",
          "[s4.t][file-change-tracker]")
{
    auto tmp = make_test_dir("deleted");
    write_file(tmp / "going_away.txt", "doomed");
    write_file(tmp / "stays.txt",       "ok");

    {
        locus::Workspace ws(tmp);
        FileChangeTracker tr;
        tr.snapshot(ws.query());

        std::error_code ec;
        fs::remove(tmp / "going_away.txt", ec);
        notify_indexer(ws, FileAction::Deleted, "going_away.txt");

        auto d = tr.diff_since_snapshot(ws.query());
        REQUIRE(contains(d.deleted, "going_away.txt"));
        REQUIRE_FALSE(contains(d.deleted, "stays.txt"));
        REQUIRE_FALSE(contains(d.changed, "going_away.txt"));
    }

    cleanup(tmp);
}

TEST_CASE("FileChangeTracker: agent-touched paths are excluded from diff",
          "[s4.t][file-change-tracker]")
{
    auto tmp = make_test_dir("touched");
    write_file(tmp / "agent_edited.txt", "v1");
    write_file(tmp / "user_edited.txt",  "v1");

    {
        locus::Workspace ws(tmp);
        FileChangeTracker tr;
        tr.snapshot(ws.query());

        mtime_gap();
        write_file(tmp / "agent_edited.txt", "v2-bigger-content-here");
        write_file(tmp / "user_edited.txt",  "v2-bigger-content-too");
        notify_indexer(ws, FileAction::Modified, "agent_edited.txt");
        notify_indexer(ws, FileAction::Modified, "user_edited.txt");
        tr.mark_agent_touched("agent_edited.txt");

        auto d = tr.diff_since_snapshot(ws.query());
        REQUIRE_FALSE(contains(d.changed, "agent_edited.txt"));
        REQUIRE(contains(d.changed, "user_edited.txt"));
    }

    cleanup(tmp);
}

TEST_CASE("FileChangeTracker: clear_agent_touched re-enables suppressed paths",
          "[s4.t][file-change-tracker]")
{
    auto tmp = make_test_dir("clear");
    write_file(tmp / "f.txt", "v1");

    {
        locus::Workspace ws(tmp);
        FileChangeTracker tr;
        tr.snapshot(ws.query());

        mtime_gap();
        write_file(tmp / "f.txt", "v2-bigger-content-here");
        notify_indexer(ws, FileAction::Modified, "f.txt");
        tr.mark_agent_touched("f.txt");
        REQUIRE(tr.agent_touched_size() == 1);

        // First diff: suppressed.
        auto d1 = tr.diff_since_snapshot(ws.query());
        REQUIRE_FALSE(contains(d1.changed, "f.txt"));

        // Clear -> next diff sees it again (snapshot still old).
        tr.clear_agent_touched();
        REQUIRE(tr.agent_touched_size() == 0);
        auto d2 = tr.diff_since_snapshot(ws.query());
        REQUIRE(contains(d2.changed, "f.txt"));
    }

    cleanup(tmp);
}

TEST_CASE("FileChangeTracker: snapshot rebases the diff baseline",
          "[s4.t][file-change-tracker]")
{
    auto tmp = make_test_dir("rebase");
    write_file(tmp / "f.txt", "v1");

    {
        locus::Workspace ws(tmp);
        FileChangeTracker tr;
        tr.snapshot(ws.query());

        mtime_gap();
        write_file(tmp / "f.txt", "v2-bigger-content-here");
        notify_indexer(ws, FileAction::Modified, "f.txt");

        REQUIRE(contains(tr.diff_since_snapshot(ws.query()).changed, "f.txt"));

        // Re-snapshot. Diff now empty -- baseline is the post-edit world.
        tr.snapshot(ws.query());
        auto d = tr.diff_since_snapshot(ws.query());
        REQUIRE(d.changed.empty());
        REQUIRE(d.deleted.empty());
    }

    cleanup(tmp);
}

TEST_CASE("FileChangeTracker: diff caps each bucket at max_per_bucket",
          "[s4.t][file-change-tracker]")
{
    auto tmp = make_test_dir("cap");
    for (int i = 0; i < 10; ++i)
        write_file(tmp / ("orig_" + std::to_string(i) + ".txt"), "x");

    {
        locus::Workspace ws(tmp);
        FileChangeTracker tr;
        tr.snapshot(ws.query());

        for (int i = 0; i < 10; ++i) {
            auto name = "new_" + std::to_string(i) + ".txt";
            write_file(tmp / name, "y");
            notify_indexer(ws, FileAction::Added, name);
        }

        auto d = tr.diff_since_snapshot(ws.query(), /*max_per_bucket=*/3);
        REQUIRE(d.changed.size() == 3);
    }

    cleanup(tmp);
}
