#include <catch2/catch_test_macros.hpp>

#include "database.h"
#include "indexer.h"
#include "workspace.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// -- Test helpers -------------------------------------------------------------

static fs::path make_test_dir(const char* suffix)
{
    auto tmp = fs::temp_directory_path() / ("locus_test_idx_" + std::string(suffix));
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
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        val = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return val;
}

static std::string query_text(sqlite3* db, const char* sql)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    std::string val;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) val = text;
    }
    sqlite3_finalize(stmt);
    return val;
}

// -- FTS5 tests ---------------------------------------------------------------

TEST_CASE("FTS5 indexes text file content", "[s0.3][indexer][fts]")
{
    auto tmp = make_test_dir("fts");

    write_file(tmp / "hello.txt", "The quick brown fox jumps over the lazy dog.");
    write_file(tmp / "world.txt", "Hello world, this is a test file for indexing.");
    write_file(tmp / "code.cpp",  "int main() { return 0; }");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        // Check files were indexed
        int count = query_int(db, "SELECT COUNT(*) FROM files WHERE is_binary = 0");
        REQUIRE(count >= 3);

        // FTS5 search for "fox"
        int fts_hits = query_int(db,
            "SELECT COUNT(*) FROM files_fts WHERE files_fts MATCH 'fox'");
        REQUIRE(fts_hits == 1);

        // FTS5 search for "test"
        fts_hits = query_int(db,
            "SELECT COUNT(*) FROM files_fts WHERE files_fts MATCH 'test'");
        REQUIRE(fts_hits >= 1);

        // FTS5 returns correct path
        std::string path = query_text(db,
            "SELECT path FROM files_fts WHERE files_fts MATCH 'fox'");
        REQUIRE(path == "hello.txt");
    }

    cleanup(tmp);
}

TEST_CASE("FTS5 porter stemming works", "[s0.3][indexer][fts]")
{
    auto tmp = make_test_dir("fts_stem");

    write_file(tmp / "doc.txt", "The workers are working on the workspace.");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        // "work" should match "workers" and "working" via porter stemmer
        int hits = query_int(db,
            "SELECT COUNT(*) FROM files_fts WHERE files_fts MATCH 'work'");
        REQUIRE(hits >= 1);
    }

    cleanup(tmp);
}

// -- Symbol extraction tests --------------------------------------------------

TEST_CASE("Tree-sitter extracts C++ symbols", "[s0.3][indexer][symbols]")
{
    auto tmp = make_test_dir("sym_cpp");

    write_file(tmp / "example.cpp", R"(
#include <string>

namespace locus {

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    void exec(const char* sql);
};

int helper_function(int x, int y)
{
    return x + y;
}

struct Config {
    int value;
};

} // namespace locus
)");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        // Should find function, class, struct, namespace
        int sym_count = query_int(db, "SELECT COUNT(*) FROM symbols");
        REQUIRE(sym_count >= 3);  // at least namespace, class, function, struct

        // Check specific symbol by name
        std::string kind = query_text(db,
            "SELECT kind FROM symbols WHERE name = 'helper_function'");
        REQUIRE(kind == "function");

        kind = query_text(db,
            "SELECT kind FROM symbols WHERE name = 'Database'");
        REQUIRE(kind == "class");

        kind = query_text(db,
            "SELECT kind FROM symbols WHERE name = 'Config'");
        REQUIRE(kind == "struct");
    }

    cleanup(tmp);
}

TEST_CASE("Tree-sitter extracts Python symbols", "[s0.3][indexer][symbols]")
{
    auto tmp = make_test_dir("sym_py");

    write_file(tmp / "example.py", R"(
class UserService:
    def __init__(self, db):
        self.db = db

    def get_user(self, user_id):
        return self.db.find(user_id)

def standalone_function(x):
    return x * 2
)");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        // Should find class + functions
        std::string kind = query_text(db,
            "SELECT kind FROM symbols WHERE name = 'UserService'");
        REQUIRE(kind == "class");

        kind = query_text(db,
            "SELECT kind FROM symbols WHERE name = 'standalone_function'");
        REQUIRE(kind == "function");
    }

    cleanup(tmp);
}

// -- Heading extraction tests -------------------------------------------------

TEST_CASE("Heading extractor finds Markdown headings", "[s0.3][indexer][headings]")
{
    auto tmp = make_test_dir("headings_md");

    write_file(tmp / "README.md", R"(# Project Title

Some intro text.

## Installation

Steps here.

### Prerequisites

- CMake
- vcpkg

## Usage

Run the thing.
)");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        int heading_count = query_int(db, "SELECT COUNT(*) FROM headings");
        REQUIRE(heading_count == 4);  // Title, Installation, Prerequisites, Usage

        // Check heading levels
        std::string text = query_text(db,
            "SELECT text FROM headings WHERE level = 1");
        REQUIRE(text == "Project Title");

        int h2_count = query_int(db,
            "SELECT COUNT(*) FROM headings WHERE level = 2");
        REQUIRE(h2_count == 2);

        int h3_count = query_int(db,
            "SELECT COUNT(*) FROM headings WHERE level = 3");
        REQUIRE(h3_count == 1);
    }

    cleanup(tmp);
}

// -- Binary detection tests ---------------------------------------------------

TEST_CASE("Binary files are detected and skipped for FTS", "[s0.3][indexer][binary]")
{
    auto tmp = make_test_dir("binary");

    // Write a text file
    write_file(tmp / "text.txt", "This is plain text.");

    // Write a binary file (contains null bytes)
    {
        std::ofstream f(tmp / "image.dat", std::ios::binary);
        char data[] = {'P', 'N', 'G', '\0', '\x89', '\x50', '\x4e', '\x47'};
        f.write(data, sizeof(data));
    }

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        // Binary file should be in files table but marked as binary
        int binary_count = query_int(db,
            "SELECT COUNT(*) FROM files WHERE is_binary = 1");
        REQUIRE(binary_count >= 1);

        // Binary file should NOT be in FTS
        int fts_count = query_int(db,
            "SELECT COUNT(*) FROM files_fts WHERE path = 'image.dat'");
        REQUIRE(fts_count == 0);

        // Text file should be in FTS
        fts_count = query_int(db,
            "SELECT COUNT(*) FROM files_fts WHERE path = 'text.txt'");
        REQUIRE(fts_count == 1);
    }

    cleanup(tmp);
}

// -- Exclude pattern tests ----------------------------------------------------

TEST_CASE("Excluded files are not indexed", "[s0.3][indexer][exclude]")
{
    auto tmp = make_test_dir("exclude");

    write_file(tmp / "src" / "main.cpp", "int main() {}");
    write_file(tmp / "build" / "output.obj", "binary stuff");
    write_file(tmp / ".git" / "config", "git config");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        // src/main.cpp should be indexed
        int count = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path LIKE '%main.cpp'");
        REQUIRE(count == 1);

        // build/ and .git/ should be excluded (default patterns)
        count = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path LIKE 'build/%'");
        REQUIRE(count == 0);

        count = query_int(db,
            "SELECT COUNT(*) FROM files WHERE path LIKE '.git/%'");
        REQUIRE(count == 0);
    }

    cleanup(tmp);
}

// -- Incremental update tests -------------------------------------------------

TEST_CASE("Incremental update re-indexes changed file", "[s0.3][indexer][incremental]")
{
    auto tmp = make_test_dir("incremental");

    write_file(tmp / "data.txt", "original content with alpha keyword");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        // Verify initial index
        int hits = query_int(db,
            "SELECT COUNT(*) FROM files_fts WHERE files_fts MATCH 'alpha'");
        REQUIRE(hits == 1);

        // Simulate a file modification event
        write_file(tmp / "data.txt", "modified content with beta keyword");
        std::vector<locus::FileEvent> events;
        events.push_back({locus::FileAction::Modified, fs::path("data.txt"), {}});
        ws.indexer().process_events(events);

        // Old content should no longer match
        hits = query_int(db,
            "SELECT COUNT(*) FROM files_fts WHERE files_fts MATCH 'alpha'");
        REQUIRE(hits == 0);

        // New content should match
        hits = query_int(db,
            "SELECT COUNT(*) FROM files_fts WHERE files_fts MATCH 'beta'");
        REQUIRE(hits == 1);
    }

    cleanup(tmp);
}

TEST_CASE("Incremental update handles file deletion", "[s0.3][indexer][incremental]")
{
    auto tmp = make_test_dir("incr_del");

    write_file(tmp / "deleteme.txt", "content to be deleted");

    {
        locus::Workspace ws(tmp);
        auto* db = ws.database().handle();

        REQUIRE(query_int(db, "SELECT COUNT(*) FROM files WHERE path = 'deleteme.txt'") == 1);

        // Delete the file and send event
        fs::remove(tmp / "deleteme.txt");
        std::vector<locus::FileEvent> events;
        events.push_back({locus::FileAction::Deleted, fs::path("deleteme.txt"), {}});
        ws.indexer().process_events(events);

        REQUIRE(query_int(db, "SELECT COUNT(*) FROM files WHERE path = 'deleteme.txt'") == 0);
        REQUIRE(query_int(db, "SELECT COUNT(*) FROM files_fts WHERE path = 'deleteme.txt'") == 0);
    }

    cleanup(tmp);
}
