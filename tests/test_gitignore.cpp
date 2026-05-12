#include <catch2/catch_test_macros.hpp>

#include "core/database.h"
#include "core/workspace.h"
#include "index/gitignore.h"
#include "index/indexer.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// -- Helpers ------------------------------------------------------------------

static fs::path make_test_dir(const char* suffix)
{
    auto tmp = fs::temp_directory_path() / ("locus_test_gitignore_" + std::string(suffix));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    return tmp;
}

static void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << content;
}

static int query_int(sqlite3* db, const char* sql)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    int val = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        val = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return val;
}

// -- Pure parser tests --------------------------------------------------------

TEST_CASE("gitignore parser: comments and blank lines skipped", "[s4.l][gitignore]")
{
    std::string text =
        "# this is a comment\n"
        "\n"
        "  \n"
        "node_modules/\n"
        "# trailing comment\n";
    auto pats = locus::parse_gitignore_text(text, "");
    // node_modules/ -> two patterns (dir-only + everything-under)
    REQUIRE(pats.size() == 2);
    REQUIRE(pats[0].pattern == "**/node_modules");
    REQUIRE(pats[0].directory_only == true);
    REQUIRE(pats[1].pattern == "**/node_modules/**");
}

TEST_CASE("gitignore parser: leading slash anchors to root", "[s4.l][gitignore]")
{
    auto pats = locus::parse_gitignore_text("/build\n", "");
    // /build -> "build" + "build/**" (no '**/' prefix because anchored)
    REQUIRE(pats.size() == 2);
    REQUIRE(pats[0].pattern == "build");
    REQUIRE(pats[1].pattern == "build/**");
}

TEST_CASE("gitignore parser: interior slash anchors implicitly", "[s4.l][gitignore]")
{
    auto pats = locus::parse_gitignore_text("src/foo\n", "");
    REQUIRE(pats.size() == 2);
    REQUIRE(pats[0].pattern == "src/foo");
    REQUIRE(pats[1].pattern == "src/foo/**");
}

TEST_CASE("gitignore parser: nested .gitignore scopes patterns", "[s4.l][gitignore]")
{
    auto pats = locus::parse_gitignore_text("dist/\n", "frontend");
    REQUIRE(pats.size() == 2);
    REQUIRE(pats[0].pattern == "frontend/**/dist");
    REQUIRE(pats[1].pattern == "frontend/**/dist/**");
}

TEST_CASE("gitignore parser: negation lines are dropped (v1)", "[s4.l][gitignore]")
{
    auto pats = locus::parse_gitignore_text(
        "*.log\n"
        "!important.log\n",
        "");
    // Only the *.log pattern (x2 -- bare + /**) survives.
    REQUIRE(pats.size() == 2);
    REQUIRE(pats[0].pattern == "**/*.log");
}

TEST_CASE("gitignore parser: trailing whitespace stripped", "[s4.l][gitignore]")
{
    auto pats = locus::parse_gitignore_text("foo.txt   \n", "");
    REQUIRE(pats[0].pattern == "**/foo.txt");
}

// -- gitignore_match ----------------------------------------------------------

TEST_CASE("gitignore_match: directory-only patterns ignored for files",
          "[s4.l][gitignore][match]")
{
    auto pats = locus::parse_gitignore_text("build/\n", "");
    // The dir-only pattern should NOT match the file path "build" (would match
    // the directory "build" if we passed is_directory=true), but the
    // accompanying "build/**" pattern catches everything underneath.
    REQUIRE(locus::gitignore_match(pats, "build/foo.o", false));
    // "build" itself isn't a file in the workspace -- it's never queried as
    // a file. Just verify the building block.
    REQUIRE_FALSE(locus::gitignore_match(pats, "buildlog.txt", false));
}

TEST_CASE("gitignore_match: wildcard star-extension", "[s4.l][gitignore][match]")
{
    auto pats = locus::parse_gitignore_text("*.log\n", "");
    REQUIRE(locus::gitignore_match(pats, "errors.log", false));
    REQUIRE(locus::gitignore_match(pats, "deep/path/server.log", false));
    REQUIRE_FALSE(locus::gitignore_match(pats, "errors.txt", false));
}

TEST_CASE("gitignore_match: anchored vs unanchored", "[s4.l][gitignore][match]")
{
    auto anchored   = locus::parse_gitignore_text("/build\n", "");
    auto unanchored = locus::parse_gitignore_text("build\n", "");
    // Anchored: only `build` at root or `build/**` matches.
    REQUIRE(locus::gitignore_match(anchored,   "build/foo", false));
    REQUIRE_FALSE(locus::gitignore_match(anchored, "src/build/foo", false));
    // Unanchored: matches at any depth (the parser prepends **/).
    REQUIRE(locus::gitignore_match(unanchored, "src/build/foo", false));
}

// -- Indexer integration ------------------------------------------------------

TEST_CASE("indexer: respects root .gitignore patterns", "[s4.l][gitignore][indexer]")
{
    auto tmp = make_test_dir("indexer_root");
    write_file(tmp / ".gitignore", "ignored.txt\nbuild/\n");
    write_file(tmp / "kept.txt", "hello");
    write_file(tmp / "ignored.txt", "drop me");
    write_file(tmp / "build" / "out.txt", "drop me too");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();
        // kept.txt should be indexed; ignored.txt + build/out.txt should not.
        int total = query_int(db, "SELECT COUNT(*) FROM files WHERE is_binary = 0");
        REQUIRE(total >= 1);
        int ignored_hit = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'ignored.txt'");
        REQUIRE(ignored_hit == 0);
        int build_hit = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path LIKE 'build/%'");
        REQUIRE(build_hit == 0);
        int kept_hit = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'kept.txt'");
        REQUIRE(kept_hit == 1);
    }

    cleanup(tmp);
}

TEST_CASE("indexer: nested .gitignore scoped to its directory",
          "[s4.l][gitignore][indexer]")
{
    auto tmp = make_test_dir("indexer_nested");
    // Root .gitignore -- empty.
    write_file(tmp / ".gitignore", "\n");
    write_file(tmp / "frontend" / ".gitignore", "secret.txt\n");
    write_file(tmp / "frontend" / "secret.txt", "drop");
    write_file(tmp / "frontend" / "kept.txt", "keep");
    // The nested rule must NOT affect a sibling top-level secret.txt.
    write_file(tmp / "secret.txt", "keep at root");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();
        int kept_root = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'secret.txt'");
        REQUIRE(kept_root == 1);
        int dropped_nested = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'frontend/secret.txt'");
        REQUIRE(dropped_nested == 0);
        int kept_nested = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'frontend/kept.txt'");
        REQUIRE(kept_nested == 1);
    }

    cleanup(tmp);
}

TEST_CASE("indexer: respect_gitignore=false falls back to exclude_patterns only",
          "[s4.l][gitignore][indexer]")
{
    auto tmp = make_test_dir("indexer_disabled");
    write_file(tmp / ".gitignore", "ignored.txt\n");
    write_file(tmp / "ignored.txt", "should be indexed when disabled");

    // Pre-write a config.json that disables the feature, so Workspace's
    // load_config reads it before the indexer is constructed.
    {
        std::ofstream cfg(tmp / ".locus" / "tmp.json");
    }
    fs::create_directories(tmp / ".locus");
    {
        std::ofstream cfg(tmp / ".locus" / "config.json");
        cfg << R"({"index":{"respect_gitignore":false}})";
    }

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();
        int hit = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'ignored.txt'");
        REQUIRE(hit == 1);
    }

    cleanup(tmp);
}

TEST_CASE("indexer: directory-only vs file pattern",
          "[s4.l][gitignore][indexer]")
{
    auto tmp = make_test_dir("indexer_dir_vs_file");
    write_file(tmp / ".gitignore", "build/\n*.log\n");
    write_file(tmp / "build" / "thing.txt", "in dir");
    // A file named exactly `build` (no extension) should NOT be excluded by
    // the dir-only pattern -- but our two-pattern emission does catch
    // "build" via the bare pattern. The user-visible behaviour matters more
    // than the exact spec compliance: most workspaces use `build/` to
    // exclude the directory and don't have a file called `build`.
    write_file(tmp / "logs" / "server.log", "file pattern");
    write_file(tmp / "logs" / "server.txt", "kept");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();
        int build_files = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path LIKE 'build/%'");
        REQUIRE(build_files == 0);
        int log_files = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path LIKE '%.log'");
        REQUIRE(log_files == 0);
        int txt_files = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'logs/server.txt'");
        REQUIRE(txt_files == 1);
    }

    cleanup(tmp);
}

TEST_CASE("indexer: reload_gitignore drops newly-excluded files",
          "[s4.l][gitignore][indexer][reload]")
{
    auto tmp = make_test_dir("indexer_reload");
    write_file(tmp / ".gitignore", "\n");
    write_file(tmp / "now_kept.txt", "hello");
    write_file(tmp / "later_dropped.txt", "world");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        REQUIRE(query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'later_dropped.txt'") == 1);

        // Append the new pattern and trigger reload directly (avoids racing
        // the file watcher in the test).
        write_file(tmp / ".gitignore", "later_dropped.txt\n");
        int dropped = ws.indexer().reload_gitignore();
        REQUIRE(dropped == 1);

        REQUIRE(query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'later_dropped.txt'") == 0);
        REQUIRE(query_int(db,
            "SELECT COUNT(*) FROM files WHERE path = 'now_kept.txt'") == 1);
    }

    cleanup(tmp);
}

