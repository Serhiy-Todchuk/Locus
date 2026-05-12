#include <catch2/catch_test_macros.hpp>

#include "agent/auto_commit.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// -- Helpers ------------------------------------------------------------------

namespace {

bool git_available()
{
    // ENOENT exit code from `git --version` is platform-specific; the simplest
    // signal is "did the call run at all". On Windows, system() returns the
    // process exit code; on POSIX, the wrapped status. Either way, 0 means
    // git ran and reported a version.
#ifdef _WIN32
    return std::system("git --version >NUL 2>&1") == 0;
#else
    return std::system("git --version >/dev/null 2>&1") == 0;
#endif
}

fs::path make_test_dir(const char* suffix)
{
    auto tmp = fs::temp_directory_path() / ("locus_test_autocommit_" + std::string(suffix));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    return tmp;
}

void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << content;
}

// Run a shell command in `cwd`. Returns process exit code; output is silenced.
int run_in(const std::string& cmd, const fs::path& cwd)
{
    std::string full;
#ifdef _WIN32
    full = "cmd /c \"cd /d \"" + cwd.string() + "\" && " + cmd + "\" >NUL 2>&1";
#else
    full = "(cd \"" + cwd.string() + "\" && " + cmd + ") >/dev/null 2>&1";
#endif
    return std::system(full.c_str());
}

void init_repo(const fs::path& root)
{
    REQUIRE(run_in("git init -q", root) == 0);
    REQUIRE(run_in("git config user.email locus-test@example.com", root) == 0);
    REQUIRE(run_in("git config user.name LocusTest", root) == 0);
    REQUIRE(run_in("git config commit.gpgsign false", root) == 0);
    // Initial commit so HEAD exists -- otherwise rev-parse can't resolve.
    write_file(root / "README.md", "init\n");
    REQUIRE(run_in("git add README.md", root) == 0);
    REQUIRE(run_in("git commit -q -m initial", root) == 0);
}

} // namespace

// -- make_commit_subject -----------------------------------------------------

TEST_CASE("make_commit_subject: empty input returns empty", "[s4.l][git][autocommit]")
{
    REQUIRE(locus::make_commit_subject("").empty());
    REQUIRE(locus::make_commit_subject("\n\n   \n").empty());
}

TEST_CASE("make_commit_subject: takes first non-empty line", "[s4.l][git][autocommit]")
{
    auto s = locus::make_commit_subject("\n\nFirst line\nSecond line\n");
    REQUIRE(s == "First line");
}

TEST_CASE("make_commit_subject: truncates with ellipsis", "[s4.l][git][autocommit]")
{
    std::string long_line(500, 'x');
    auto s = locus::make_commit_subject(long_line, 50);
    REQUIRE(s.size() == 50);
    REQUIRE(s.substr(s.size() - 3) == "...");
}

TEST_CASE("make_commit_subject: short input passes through", "[s4.l][git][autocommit]")
{
    auto s = locus::make_commit_subject("Short message", 200);
    REQUIRE(s == "Short message");
}

TEST_CASE("make_commit_subject: trims trailing whitespace before ellipsis",
          "[s4.l][git][autocommit]")
{
    auto s = locus::make_commit_subject("hello                          there", 12);
    // Body cap = 12 - 3 (ellipsis) = 9 chars: "hello    " -> trim -> "hello..."
    REQUIRE(s == "hello...");
}

// -- auto_commit_after_turn integration --------------------------------------

TEST_CASE("auto_commit: skips silently when not a git repo",
          "[s4.l][git][autocommit]")
{
    auto tmp = make_test_dir("not_a_repo");
    write_file(tmp / "file.txt", "content");

    auto r = locus::auto_commit_after_turn(tmp, "[locus] ", "", "Made a change");
    REQUIRE(r.success);
    REQUIRE(r.skipped);
    REQUIRE(r.short_sha.empty());

    cleanup(tmp);
}

TEST_CASE("auto_commit: commits a tracked change to current branch",
          "[s4.l][git][autocommit]")
{
    if (!git_available()) {
        SUCCEED("git not on PATH; skipping");
        return;
    }

    auto tmp = make_test_dir("happy_path");
    init_repo(tmp);

    write_file(tmp / "agent_added.txt", "agent wrote this");

    auto r = locus::auto_commit_after_turn(tmp, "[locus] ", "",
                                            "Added a new file for testing");
    REQUIRE(r.success);
    REQUIRE_FALSE(r.skipped);
    REQUIRE_FALSE(r.short_sha.empty());
    REQUIRE_FALSE(r.branch.empty());

    cleanup(tmp);
}

TEST_CASE("auto_commit: empty assistant message skips commit",
          "[s4.l][git][autocommit]")
{
    if (!git_available()) {
        SUCCEED("git not on PATH; skipping");
        return;
    }

    auto tmp = make_test_dir("empty_msg");
    init_repo(tmp);
    write_file(tmp / "noise.txt", "stuff");

    auto r = locus::auto_commit_after_turn(tmp, "[locus] ", "", "");
    REQUIRE(r.success);
    REQUIRE(r.skipped);
    REQUIRE(r.short_sha.empty());

    cleanup(tmp);
}

TEST_CASE("auto_commit: nothing-to-commit is treated as skipped (not failure)",
          "[s4.l][git][autocommit]")
{
    if (!git_available()) {
        SUCCEED("git not on PATH; skipping");
        return;
    }

    auto tmp = make_test_dir("nothing_to_commit");
    init_repo(tmp);
    // No new changes after the initial commit.

    auto r = locus::auto_commit_after_turn(tmp, "[locus] ", "",
                                            "I think I changed something");
    REQUIRE(r.success);
    REQUIRE(r.skipped);

    cleanup(tmp);
}

TEST_CASE("auto_commit: creates branch when commit_branch missing",
          "[s4.l][git][autocommit]")
{
    if (!git_available()) {
        SUCCEED("git not on PATH; skipping");
        return;
    }

    auto tmp = make_test_dir("create_branch");
    init_repo(tmp);
    write_file(tmp / "new.txt", "land me on side branch");

    auto r = locus::auto_commit_after_turn(tmp, "[locus] ", "locus-agent",
                                            "Side-branch commit");
    REQUIRE(r.success);
    REQUIRE(r.branch == "locus-agent");
    REQUIRE_FALSE(r.short_sha.empty());

    cleanup(tmp);
}

TEST_CASE("auto_commit: prefix is included in subject", "[s4.l][git][autocommit]")
{
    if (!git_available()) {
        SUCCEED("git not on PATH; skipping");
        return;
    }

    auto tmp = make_test_dir("prefix_check");
    init_repo(tmp);
    write_file(tmp / "x.txt", "anything");

    auto r = locus::auto_commit_after_turn(tmp, "[agent] ", "",
                                            "Did the thing");
    REQUIRE(r.success);

    // Read the latest commit message and assert the prefix.
#ifdef _WIN32
    std::string cmd = "cmd /c \"cd /d \"" + tmp.string()
                    + "\" && git log -1 --pretty=%s > \"" + tmp.string()
                    + "\\last_msg.txt\" 2>NUL\"";
#else
    std::string cmd = "(cd \"" + tmp.string()
                    + "\" && git log -1 --pretty=%s) > \"" + tmp.string()
                    + "/last_msg.txt\" 2>/dev/null";
#endif
    std::system(cmd.c_str());
    std::ifstream in(tmp / "last_msg.txt");
    std::string last;
    std::getline(in, last);
    while (!last.empty() && (last.back() == '\r' || last.back() == '\n'))
        last.pop_back();
    REQUIRE(last.rfind("[agent] ", 0) == 0);
    REQUIRE(last.find("Did the thing") != std::string::npos);

    cleanup(tmp);
}
