// S4.W tests -- filesystem-sourced ListDirectoryTool with index annotation.
//
// These cases drive the real tool against a real Workspace so the index is
// populated and the cross-reference path is exercised end-to-end. The legacy
// happy-path coverage lives in `test_tools.cpp::ListDirectoryTool: returns
// indexed files at root with '.'` -- this file adds the new annotation, depth,
// and truncation behaviour.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "core/workspace.h"
#include "tools/index_tools.h"
#include "tools/tool.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path make_test_dir(const std::string& tag)
{
    auto p = fs::temp_directory_path() / ("locus_test_s4w_" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

// Tiny PNG header bytes so the indexer's binary detector flags them and the
// `.png` extension lands in the binary-list path too.
void write_png_stub(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    static const unsigned char header[] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
        0, 0, 0, 13, 'I', 'H', 'D', 'R',
        0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0,
    };
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
}

locus::ToolResult invoke_list(locus::Workspace& ws,
                              const std::string& path = "",
                              int depth = 0,
                              int max_entries = 200)
{
    locus::IWorkspaceServices& services = ws;
    locus::ListDirectoryTool tool;
    nlohmann::json args = nlohmann::json::object();
    if (!path.empty()) args["path"] = path;
    if (depth != 0)    args["depth"] = depth;
    if (max_entries != 200) args["max_entries"] = max_entries;
    locus::ToolCall call{"c", "list_directory", args};
    return tool.execute(call, services);
}

} // namespace

// -- 1. Indexed text files keep their existing [lang] annotation --------------

TEST_CASE("ListDirectoryTool: indexed text files annotated with language",
          "[s4.w][list_directory]")
{
    auto tmp = make_test_dir("indexed_lang");
    write_file(tmp / "hello.cpp", "int main() { return 0; }");
    write_file(tmp / "readme.md",  "# title");

    {
        locus::Workspace ws(tmp);
        auto r = invoke_list(ws);
        REQUIRE(r.success);
        REQUIRE_THAT(r.content, ContainsSubstring("hello.cpp"));
        REQUIRE_THAT(r.content, ContainsSubstring("[cpp]"));
        REQUIRE_THAT(r.content, ContainsSubstring("readme.md"));
        REQUIRE_THAT(r.content, ContainsSubstring("[markdown]"));
    }
    fs::remove_all(tmp);
}

// -- 2. Binary-extension files surface as [binary] ----------------------------

TEST_CASE("ListDirectoryTool: PNG and ZIP-extension files are [binary]",
          "[s4.w][list_directory]")
{
    auto tmp = make_test_dir("binary_ext");
    write_png_stub(tmp / "logo.png");
    write_file(tmp / "archive.zip", std::string("PK\x03\x04stub-zip-bytes"));
    write_file(tmp / "doc.pdf",     std::string("%PDF-1.4 not-a-real-pdf"));
    write_file(tmp / "code.cpp",    "int x;");

    {
        locus::Workspace ws(tmp);
        auto r = invoke_list(ws);
        REQUIRE(r.success);
        REQUIRE_THAT(r.content, ContainsSubstring("logo.png"));
        REQUIRE_THAT(r.content, ContainsSubstring("[binary]"));
        REQUIRE_THAT(r.content, ContainsSubstring("archive.zip"));
        REQUIRE_THAT(r.content, ContainsSubstring("doc.pdf"));
    }
    fs::remove_all(tmp);
}

// -- 3. Default workspace excludes stay hidden --------------------------------

TEST_CASE("ListDirectoryTool: .git and node_modules stay hidden",
          "[s4.w][list_directory]")
{
    auto tmp = make_test_dir("excluded");
    write_file(tmp / "src" / "main.cpp", "int main() {}");
    write_file(tmp / ".git" / "HEAD", "ref: refs/heads/main");
    write_file(tmp / ".git" / "config", "[core]");
    write_file(tmp / "node_modules" / "pkg" / "index.js", "module.exports = {};");

    {
        locus::Workspace ws(tmp);
        auto r = invoke_list(ws, "", /*depth=*/3);
        REQUIRE(r.success);
        REQUIRE_THAT(r.content, ContainsSubstring("main.cpp"));
        // Neither directory nor anything inside should leak.
        REQUIRE_FALSE(r.content.find(".git") != std::string::npos);
        REQUIRE_FALSE(r.content.find("node_modules") != std::string::npos);
    }
    fs::remove_all(tmp);
}

// -- 4. Files past the indexer size cap surface as [oversized] ----------------

TEST_CASE("ListDirectoryTool: file past max_file_size_kb shows as [oversized]",
          "[s4.w][list_directory]")
{
    auto tmp = make_test_dir("oversized");

    // 4 KB of plain text, well above our 1 KB cap below.
    std::string big_text;
    big_text.reserve(4096);
    for (int i = 0; i < 4096; ++i) big_text.push_back('a' + (i % 26));

    write_file(tmp / "huge.txt",  big_text);
    write_file(tmp / "tiny.txt",  "hello");

    {
        locus::Workspace ws(tmp);
        ws.config().index.max_file_size_kb = 1;
        auto r = invoke_list(ws);
        REQUIRE(r.success);
        REQUIRE_THAT(r.content, ContainsSubstring("huge.txt"));
        REQUIRE_THAT(r.content, ContainsSubstring("[oversized]"));
        REQUIRE_THAT(r.content, ContainsSubstring("tiny.txt"));
    }
    fs::remove_all(tmp);
}

// -- 5. Unknown-extension non-indexed file shows as [unindexed] ---------------
//
// We use a freshly created file inside an existing Workspace: the
// constructor-time `build_initial` ran before the file existed, and the
// watcher's debounce window means the file isn't in the index yet when we
// list. The annotation must surface `[unindexed]` rather than guessing.

TEST_CASE("ListDirectoryTool: unknown-extension file appears as [unindexed]",
          "[s4.w][list_directory]")
{
    auto tmp = make_test_dir("unindexed");
    write_file(tmp / "seed.md", "# seed");

    {
        locus::Workspace ws(tmp);
        // Drop a file with an extension the indexer has no extractor for and
        // that's not in the binary list. Created post-construction so it isn't
        // in the index yet.
        write_file(tmp / "mystery.qqz", "could be anything");

        auto r = invoke_list(ws);
        REQUIRE(r.success);
        REQUIRE_THAT(r.content, ContainsSubstring("mystery.qqz"));
        REQUIRE_THAT(r.content, ContainsSubstring("[unindexed]"));
    }
    fs::remove_all(tmp);
}

// -- 6. Truncation footer fires at max_entries --------------------------------

TEST_CASE("ListDirectoryTool: hitting max_entries emits truncation footer",
          "[s4.w][list_directory]")
{
    auto tmp = make_test_dir("truncate");
    for (int i = 0; i < 10; ++i) {
        write_file(tmp / ("f" + std::to_string(i) + ".txt"), "x");
    }

    {
        locus::Workspace ws(tmp);
        auto r = invoke_list(ws, "", /*depth=*/0, /*max_entries=*/3);
        REQUIRE(r.success);
        REQUIRE_THAT(r.content, ContainsSubstring("more entries truncated"));
    }
    fs::remove_all(tmp);
}

// -- 7. depth=0 vs depth=N --------------------------------------------------

TEST_CASE("ListDirectoryTool: depth=0 shows direct children only; depth=2 recurses",
          "[s4.w][list_directory]")
{
    auto tmp = make_test_dir("depth");
    write_file(tmp / "root.cpp", "int x;");
    write_file(tmp / "sub"      / "mid.cpp",   "int y;");
    write_file(tmp / "sub" / "deeper" / "leaf.cpp", "int z;");

    {
        locus::Workspace ws(tmp);

        auto r0 = invoke_list(ws, "", /*depth=*/0);
        REQUIRE(r0.success);
        REQUIRE_THAT(r0.content, ContainsSubstring("root.cpp"));
        REQUIRE_THAT(r0.content, ContainsSubstring("sub/"));
        // depth=0 should not list any path *inside* sub/.
        REQUIRE_FALSE(r0.content.find("mid.cpp") != std::string::npos);

        auto r2 = invoke_list(ws, "", /*depth=*/2);
        REQUIRE(r2.success);
        REQUIRE_THAT(r2.content, ContainsSubstring("root.cpp"));
        REQUIRE_THAT(r2.content, ContainsSubstring("mid.cpp"));
        REQUIRE_THAT(r2.content, ContainsSubstring("leaf.cpp"));
    }
    fs::remove_all(tmp);
}
