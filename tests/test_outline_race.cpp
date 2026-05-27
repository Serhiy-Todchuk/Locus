// Investigation: agentic finding claims `get_file_outline` returns 0 entries
// for newly-created files that "clearly have top-level functions". Hypothesis:
// the watcher pump's 1.5 s quiet-period batches indexing, so an outline call
// fired between two agent rounds (e.g. write_file in round N, get_file_outline
// in round N+1) hits the symbols table BEFORE the file has been indexed.
//
// The user-turn flush (`WatcherPump::flush_now()` at prompt submit, both in
// `main.cpp` and `locus_frame.cpp`) only happens once per USER turn, not per
// agent round. Mid-turn file creation -> mid-turn outline query is the race.
//
// This test reproduces the race deterministically and confirms `flush_now()`
// is the fix.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "core/workspace.h"
#include "core/watcher_pump.h"
#include "index/index_query.h"
#include "tools/index_tools.h"
#include "tools/tool.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path make_test_dir(const char* tag)
{
    auto p = fs::temp_directory_path() / ("locus_test_outline_race_" + std::string(tag));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

void write_text(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

locus::ToolResult call_outline(locus::Workspace& ws, const std::string& path)
{
    locus::IWorkspaceServices& services = ws;
    locus::GetFileOutlineTool tool;
    nlohmann::json args;
    args["path"] = path;
    locus::ToolCall call{"c", "get_file_outline", args};
    return tool.execute(call, services);
}

// A snake.cpp shaped like the one in the agentic finding: top-level free
// functions, a struct, and main(). cpp_extractor.cpp registers rules for
// function_definition + struct_specifier, so every symbol below SHOULD be
// surfaced once the file is indexed.
constexpr const char* k_snake_cpp = R"(#include <cstdio>

struct Point {
    int x;
    int y;
};

void tick() {
    std::printf("tick\n");
}

int score(int apples) {
    return apples * 10;
}

int main() {
    tick();
    return score(3);
}
)";

} // namespace

// -- Baseline ----------------------------------------------------------------
// A file present BEFORE the workspace is opened goes through the ctor's
// blocking initial index, so the outline works immediately. This proves the
// extractor rules cover the file's shape -- isolating any mid-turn failure
// to the indexing-freshness axis, not a schema issue.

TEST_CASE("get_file_outline: baseline - pre-existing file works",
          "[outline-race][baseline]")
{
    auto tmp = make_test_dir("baseline");
    write_text(tmp / "snake.cpp", k_snake_cpp);

    {
        locus::Workspace ws(tmp);

        auto r = call_outline(ws, "snake.cpp");
        REQUIRE(r.success);

        INFO("outline body:\n" << r.content);
        // Free functions + struct should all land.
        REQUIRE_THAT(r.content, ContainsSubstring("tick"));
        REQUIRE_THAT(r.content, ContainsSubstring("score"));
        REQUIRE_THAT(r.content, ContainsSubstring("main"));
        REQUIRE_THAT(r.content, ContainsSubstring("Point"));
        // The "0 entries" footprint that the agentic finding flagged.
        REQUIRE_THAT(r.content, !ContainsSubstring("(0 entries)"));
        // Header carries the line count for indexed files.
        REQUIRE_THAT(r.content, ContainsSubstring("lines)"));
    }

    fs::remove_all(tmp);
}

// -- Line-count header on the outline ----------------------------------------
// Tightly-controlled file so the assertion can pin the exact number.

TEST_CASE("get_file_outline: header includes recorded line count",
          "[outline][line_count]")
{
    auto tmp = make_test_dir("linecount_outline");

    // Exactly 10 lines, trailing newline -- indexer reports 10.
    std::string body;
    body += "int alpha() { return 1; }\n";
    body += "int beta()  { return 2; }\n";
    body += "int gamma() { return 3; }\n";
    body += "int delta() { return 4; }\n";
    body += "int eps()   { return 5; }\n";
    body += "int zeta()  { return 6; }\n";
    body += "int eta()   { return 7; }\n";
    body += "int theta() { return 8; }\n";
    body += "int iota()  { return 9; }\n";
    body += "int main()  { return 0; }\n";
    write_text(tmp / "ten.cpp", body);

    {
        locus::Workspace ws(tmp);
        auto r = call_outline(ws, "ten.cpp");
        REQUIRE(r.success);
        INFO("outline body:\n" << r.content);
        REQUIRE_THAT(r.content, ContainsSubstring("(10 lines)"));
    }

    fs::remove_all(tmp);
}

// -- The race ----------------------------------------------------------------
// A file created AFTER workspace ctor relies on the watcher pump to drain the
// event into the indexer. The pump's quiet-period is 1.5 s by default, so an
// outline call fired immediately after the write hits an empty symbols table.

TEST_CASE("get_file_outline: race - mid-turn write returns actionable empty-state message",
          "[outline-race][bug]")
{
    auto tmp = make_test_dir("race");
    {
        locus::Workspace ws(tmp);

        // Workspace is open + indexed (empty). Now drop a file -- this is what
        // happens when an external editor writes the file mid-conversation
        // (and would also happen if the dispatcher's post-mutation flush
        // weren't in place after a tool-driven write_file).
        write_text(tmp / "snake.cpp", k_snake_cpp);

        // Give efsw a moment to deliver the kernel event to FileWatcher's
        // internal queue. We do NOT call flush_now() -- exercises the empty-
        // state messaging path (the watcher pump's 1.5 s quiet-period means
        // process_events hasn't fired yet).
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        auto r = call_outline(ws, "snake.cpp");
        REQUIRE(r.success);  // tool itself succeeds

        INFO("outline body (race):\n" << r.content);
        // After the fix: the empty-state branch explains why, instead of the
        // misleading "0 entries" line that looked identical to "extractor
        // saw the file and found nothing" or "wrong path".
        REQUIRE_THAT(r.content, ContainsSubstring("no symbols or headings"));
        REQUIRE_THAT(r.content, ContainsSubstring("present on disk"));
        REQUIRE_THAT(r.content, ContainsSubstring("indexing has not caught up"));
    }

    fs::remove_all(tmp);
}

// -- The fix -----------------------------------------------------------------
// flush_now() drains the watcher synchronously and runs process_events on the
// calling thread. After it returns, the symbols table is populated and the
// outline call works.

TEST_CASE("get_file_outline: flush_now resolves the race",
          "[outline-race][fix]")
{
    auto tmp = make_test_dir("flush");
    {
        locus::Workspace ws(tmp);

        write_text(tmp / "snake.cpp", k_snake_cpp);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // The fix: synchronous drain + index before reading symbols.
        ws.watcher_pump().flush_now();

        auto r = call_outline(ws, "snake.cpp");
        REQUIRE(r.success);

        INFO("outline body (after flush):\n" << r.content);
        REQUIRE_THAT(r.content, ContainsSubstring("tick"));
        REQUIRE_THAT(r.content, ContainsSubstring("score"));
        REQUIRE_THAT(r.content, ContainsSubstring("main"));
        REQUIRE_THAT(r.content, ContainsSubstring("Point"));
        REQUIRE_THAT(r.content, !ContainsSubstring("(0 entries)"));
    }

    fs::remove_all(tmp);
}

// -- Distinguished failure UX -------------------------------------------------
// Before the fix every "no symbols for this path" path produced the same
// `outline (0 entries)` string -- agent + human triaging couldn't tell apart:
//   - file exists but isn't indexed yet (the race above)
//   - file is over `max_file_size_kb` and was skipped at index time
//   - path doesn't exist at all
//   - file IS indexed but has no extractor-matching nodes
// After the fix each case carries a distinct, actionable message.

TEST_CASE("get_file_outline: nonexistent path now returns a clear error",
          "[outline-race][silent-failure]")
{
    auto tmp = make_test_dir("missing");
    {
        locus::Workspace ws(tmp);

        auto r = call_outline(ws, "does_not_exist.cpp");
        REQUIRE_FALSE(r.success);
        INFO("outline body (nonexistent):\n" << r.content);
        REQUIRE_THAT(r.content, ContainsSubstring("file not found"));
    }
    fs::remove_all(tmp);
}
