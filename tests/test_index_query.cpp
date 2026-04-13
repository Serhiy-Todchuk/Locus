#include <catch2/catch_test_macros.hpp>

#include "database.h"
#include "index_query.h"
#include "indexer.h"
#include "workspace.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// -- Test helpers -------------------------------------------------------------

static fs::path make_test_dir(const char* suffix)
{
    auto tmp = fs::temp_directory_path() / ("locus_test_iq_" + std::string(suffix));
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

// -- search_text tests --------------------------------------------------------

TEST_CASE("search_text returns ranked results", "[s0.4][index_query][search_text]")
{
    auto tmp = make_test_dir("st_basic");

    write_file(tmp / "alpha.txt", "The database connection is important for performance.");
    write_file(tmp / "beta.txt", "Performance tuning and database optimization guide.");
    write_file(tmp / "gamma.txt", "Unrelated content about cooking recipes.");

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        auto results = q.search_text("database");
        REQUIRE(results.size() >= 2);

        // Both files mentioning "database" should appear
        bool found_alpha = false, found_beta = false;
        for (auto& r : results) {
            if (r.path == "alpha.txt") found_alpha = true;
            if (r.path == "beta.txt") found_beta = true;
            // Score should be non-zero
            REQUIRE(r.score != 0.0);
        }
        REQUIRE(found_alpha);
        REQUIRE(found_beta);

        // "cooking" should only match gamma
        auto cooking = q.search_text("cooking");
        REQUIRE(cooking.size() == 1);
        REQUIRE(cooking[0].path == "gamma.txt");
    }

    cleanup(tmp);
}

TEST_CASE("search_text respects max_results", "[s0.4][index_query][search_text]")
{
    auto tmp = make_test_dir("st_limit");

    for (int i = 0; i < 10; ++i) {
        write_file(tmp / ("file" + std::to_string(i) + ".txt"),
                   "common keyword appears in every file " + std::to_string(i));
    }

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        locus::SearchOptions opts;
        opts.max_results = 3;
        auto results = q.search_text("keyword", opts);
        REQUIRE(results.size() == 3);
    }

    cleanup(tmp);
}

TEST_CASE("search_text returns line and snippet", "[s0.4][index_query][search_text]")
{
    auto tmp = make_test_dir("st_snippet");

    write_file(tmp / "code.cpp", "line one\nline two\nint target_function() {\n    return 42;\n}\n");

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        auto results = q.search_text("target_function");
        REQUIRE(results.size() == 1);
        REQUIRE(results[0].line == 3);
        REQUIRE(results[0].snippet.find("target_function") != std::string::npos);
    }

    cleanup(tmp);
}

// -- search_symbols tests -----------------------------------------------------

TEST_CASE("search_symbols finds by prefix", "[s0.4][index_query][search_symbols]")
{
    auto tmp = make_test_dir("ss_prefix");

    write_file(tmp / "example.cpp", R"(
int helper_one(int x) { return x; }
int helper_two(int x) { return x * 2; }
int unrelated(int x) { return x + 1; }
)");

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        auto results = q.search_symbols("helper");
        REQUIRE(results.size() == 2);
        REQUIRE(results[0].name.find("helper") == 0);
        REQUIRE(results[1].name.find("helper") == 0);

        // Exact prefix should narrow results
        auto exact = q.search_symbols("helper_one");
        REQUIRE(exact.size() == 1);
        REQUIRE(exact[0].name == "helper_one");
        REQUIRE(exact[0].kind == "function");
        REQUIRE(exact[0].line_start > 0);
    }

    cleanup(tmp);
}

TEST_CASE("search_symbols filters by kind", "[s0.4][index_query][search_symbols]")
{
    auto tmp = make_test_dir("ss_kind");

    write_file(tmp / "mixed.cpp", R"(
class MyClass {
public:
    void do_stuff() {}
};

struct MyStruct {
    int value;
};

int my_func() { return 0; }
)");

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        // Filter kind = "class"
        auto classes = q.search_symbols("My", "class");
        REQUIRE(classes.size() == 1);
        REQUIRE(classes[0].name == "MyClass");

        // Filter kind = "struct"
        auto structs = q.search_symbols("My", "struct");
        REQUIRE(structs.size() == 1);
        REQUIRE(structs[0].name == "MyStruct");
    }

    cleanup(tmp);
}

TEST_CASE("search_symbols filters by language", "[s0.4][index_query][search_symbols]")
{
    auto tmp = make_test_dir("ss_lang");

    write_file(tmp / "code.cpp", "int compute(int x) { return x; }");
    write_file(tmp / "code.py", "def compute(x):\n    return x\n");

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        auto cpp_only = q.search_symbols("compute", "", "cpp");
        REQUIRE(cpp_only.size() == 1);
        REQUIRE(cpp_only[0].language == "cpp");

        auto py_only = q.search_symbols("compute", "", "python");
        REQUIRE(py_only.size() == 1);
        REQUIRE(py_only[0].language == "python");
    }

    cleanup(tmp);
}

// -- get_file_outline tests ---------------------------------------------------

TEST_CASE("get_file_outline returns headings and symbols sorted by line", "[s0.4][index_query][outline]")
{
    auto tmp = make_test_dir("outline");

    write_file(tmp / "doc.md", "# Title\n\nIntro.\n\n## Section A\n\nText.\n\n## Section B\n\nMore text.\n");
    write_file(tmp / "code.cpp", R"(
struct Config {
    int x;
};

int process(int v) {
    return v + 1;
}
)");

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        // Markdown outline
        auto md_outline = q.get_file_outline("doc.md");
        REQUIRE(md_outline.size() == 3);
        REQUIRE(md_outline[0].type == locus::OutlineEntry::Heading);
        REQUIRE(md_outline[0].text == "Title");
        REQUIRE(md_outline[0].level == 1);
        REQUIRE(md_outline[1].text == "Section A");
        REQUIRE(md_outline[2].text == "Section B");
        // Should be sorted by line
        REQUIRE(md_outline[0].line < md_outline[1].line);
        REQUIRE(md_outline[1].line < md_outline[2].line);

        // C++ outline
        auto cpp_outline = q.get_file_outline("code.cpp");
        REQUIRE(cpp_outline.size() >= 2);
        bool found_struct = false, found_func = false;
        for (auto& e : cpp_outline) {
            REQUIRE(e.type == locus::OutlineEntry::Symbol);
            if (e.text == "Config") found_struct = true;
            if (e.text == "process") found_func = true;
        }
        REQUIRE(found_struct);
        REQUIRE(found_func);

        // Non-existent file returns empty
        auto empty = q.get_file_outline("nonexistent.txt");
        REQUIRE(empty.empty());
    }

    cleanup(tmp);
}

// -- list_directory tests -----------------------------------------------------

TEST_CASE("list_directory returns files and directories", "[s0.4][index_query][list_dir]")
{
    auto tmp = make_test_dir("listdir");

    write_file(tmp / "root.txt", "root file");
    write_file(tmp / "src" / "main.cpp", "int main() {}");
    write_file(tmp / "src" / "util.cpp", "void util() {}");
    write_file(tmp / "docs" / "readme.md", "# Docs");
    write_file(tmp / "src" / "sub" / "deep.cpp", "void deep() {}");

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        // Root listing, depth=0
        auto root_entries = q.list_directory("", 0);
        // Should contain root.txt + dirs (src, docs)
        bool found_root_txt = false, found_src = false, found_docs = false;
        for (auto& e : root_entries) {
            if (e.path == "root.txt") found_root_txt = true;
            if (e.path == "src" && e.is_directory) found_src = true;
            if (e.path == "docs" && e.is_directory) found_docs = true;
        }
        REQUIRE(found_root_txt);
        REQUIRE(found_src);
        REQUIRE(found_docs);

        // Directories should sort before files
        if (root_entries.size() >= 2) {
            bool dirs_first = true;
            bool seen_file = false;
            for (auto& e : root_entries) {
                if (!e.is_directory) seen_file = true;
                if (e.is_directory && seen_file) dirs_first = false;
            }
            REQUIRE(dirs_first);
        }

        // src/ listing, depth=0
        auto src_entries = q.list_directory("src", 0);
        bool found_main = false, found_util = false, found_sub = false;
        for (auto& e : src_entries) {
            if (e.path == "src/main.cpp") found_main = true;
            if (e.path == "src/util.cpp") found_util = true;
            if (e.path == "src/sub" && e.is_directory) found_sub = true;
        }
        REQUIRE(found_main);
        REQUIRE(found_util);
        REQUIRE(found_sub);

        // src/ listing, depth=1 should include deep files
        auto src_deep = q.list_directory("src", 1);
        bool found_deep = false;
        for (auto& e : src_deep) {
            if (e.path == "src/sub/deep.cpp") found_deep = true;
        }
        REQUIRE(found_deep);
    }

    cleanup(tmp);
}

TEST_CASE("list_directory returns file metadata", "[s0.4][index_query][list_dir]")
{
    auto tmp = make_test_dir("listdir_meta");

    write_file(tmp / "data.json", R"({"key": "value"})");

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        auto entries = q.list_directory("", 0);
        bool found = false;
        for (auto& e : entries) {
            if (e.path == "data.json") {
                found = true;
                REQUIRE(e.ext == ".json");
                REQUIRE(e.language == "json");
                REQUIRE(e.size_bytes > 0);
                REQUIRE(e.modified_at > 0);
                REQUIRE_FALSE(e.is_binary);
                REQUIRE_FALSE(e.is_directory);
            }
        }
        REQUIRE(found);
    }

    cleanup(tmp);
}

// -- Performance test ---------------------------------------------------------

TEST_CASE("All queries return in under 100ms on moderate workspace", "[s0.4][index_query][perf]")
{
    auto tmp = make_test_dir("perf");

    // Create a workspace with enough files to be a meaningful perf test.
    // 500 files is a reasonable proxy (10k would be too slow to set up in tests).
    for (int i = 0; i < 500; ++i) {
        std::string dir = "dir" + std::to_string(i / 50);
        std::string name = "file" + std::to_string(i) + ".txt";
        write_file(tmp / dir / name,
                   "Content of file number " + std::to_string(i) +
                   " with searchable keyword and various terms. "
                   "Function definitions and class declarations appear here.");
    }
    // Add some code files for symbol queries
    for (int i = 0; i < 20; ++i) {
        write_file(tmp / "code" / ("module" + std::to_string(i) + ".cpp"),
                   "int func_" + std::to_string(i) + "(int x) { return x + " + std::to_string(i) + "; }\n"
                   "struct Data_" + std::to_string(i) + " { int val; };\n");
    }

    {
        locus::Workspace ws(tmp);
        auto& q = ws.query();

        auto time_it = [](auto&& fn) {
            auto t0 = std::chrono::steady_clock::now();
            fn();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0);
            return elapsed.count();
        };

        // search_text
        long ms = time_it([&]{ q.search_text("keyword"); });
        REQUIRE(ms < 100);

        // search_symbols
        ms = time_it([&]{ q.search_symbols("func_"); });
        REQUIRE(ms < 100);

        // get_file_outline
        ms = time_it([&]{ q.get_file_outline("code/module0.cpp"); });
        REQUIRE(ms < 100);

        // list_directory
        ms = time_it([&]{ q.list_directory("", 0); });
        REQUIRE(ms < 100);
    }

    cleanup(tmp);
}
