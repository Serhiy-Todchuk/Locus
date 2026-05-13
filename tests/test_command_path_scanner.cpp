// S4.V Task 5 -- outside-workspace path scanner. Pure-function unit tests;
// the scanner runs at every `run_command` / `run_command_bg` dispatch on the
// agent thread so even a small regression compounds quickly.

#include <catch2/catch_test_macros.hpp>

#include "tools/command_path_scanner.h"

#include <filesystem>

namespace fs = std::filesystem;
using locus::tools::scan_outside_workspace_paths;

namespace {

fs::path make_temp_ws(const char* suffix)
{
    auto p = fs::temp_directory_path() / (std::string("locus_cps_") + suffix);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

} // namespace

TEST_CASE("empty command returns no flags", "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("empty");
    REQUIRE(scan_outside_workspace_paths("", ws).empty());
    REQUIRE(scan_outside_workspace_paths("npm test", ws).empty());
}

TEST_CASE("workspace-relative path is not flagged", "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("rel");
    auto flagged = scan_outside_workspace_paths("cat src/main.cpp", ws);
    REQUIRE(flagged.empty());
}

TEST_CASE("absolute Windows path outside workspace is flagged",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("abs");
    auto flagged = scan_outside_workspace_paths(
        "del C:\\Users\\me\\Documents\\notes.txt", ws);
    REQUIRE(flagged.size() == 1);
    REQUIRE(flagged[0] == "C:\\Users\\me\\Documents\\notes.txt");
}

TEST_CASE("absolute Windows path INSIDE workspace is not flagged",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("abs_inside");
    fs::path inside = ws / "src" / "foo.cpp";
    auto flagged = scan_outside_workspace_paths(
        "cat " + inside.string(), ws);
    REQUIRE(flagged.empty());
}

TEST_CASE("parent-relative path that escapes the workspace is flagged",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("parent");
    auto flagged = scan_outside_workspace_paths(
        "cp ..\\..\\other-repo\\secret.key .", ws);
    REQUIRE(flagged.size() == 1);
    REQUIRE(flagged[0] == "..\\..\\other-repo\\secret.key");
}

TEST_CASE("parent-relative path that stays inside workspace is NOT flagged",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("inner");
    // src/foo/../bar.txt canonicalises to src/bar.txt, which is inside ws.
    auto flagged = scan_outside_workspace_paths(
        "cat src/foo/../bar.txt", ws);
    REQUIRE(flagged.empty());
}

TEST_CASE("environment variable expansions are flagged unconditionally",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("env");
    auto a = scan_outside_workspace_paths(
        "type %USERPROFILE%\\.ssh\\id_rsa", ws);
    REQUIRE(a.size() == 1);
    REQUIRE(a[0] == "%USERPROFILE%\\.ssh\\id_rsa");

    auto b = scan_outside_workspace_paths("cat $HOME/.bashrc", ws);
    REQUIRE(b.size() == 1);
    REQUIRE(b[0] == "$HOME/.bashrc");

    auto c = scan_outside_workspace_paths("cat ~/.bashrc", ws);
    REQUIRE(c.size() == 1);
    REQUIRE(c[0] == "~/.bashrc");
}

TEST_CASE("multiple offending tokens are all flagged in order",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("multi");
    auto flagged = scan_outside_workspace_paths(
        "copy C:\\Users\\me\\a.txt D:\\b.txt && del ..\\..\\c.txt", ws);
    REQUIRE(flagged.size() == 3);
    REQUIRE(flagged[0] == "C:\\Users\\me\\a.txt");
    REQUIRE(flagged[1] == "D:\\b.txt");
    REQUIRE(flagged[2] == "..\\..\\c.txt");
}

TEST_CASE("quoted absolute paths are flagged with quotes stripped",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("quoted");
    auto flagged = scan_outside_workspace_paths(
        "del \"C:\\Program Files\\App\\config.ini\"", ws);
    REQUIRE(flagged.size() == 1);
    REQUIRE(flagged[0] == "C:\\Program Files\\App\\config.ini");
}

TEST_CASE("UNC path is flagged", "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("unc");
    auto flagged = scan_outside_workspace_paths(
        "dir \\\\server\\share\\foo", ws);
    REQUIRE(flagged.size() == 1);
    REQUIRE(flagged[0] == "\\\\server\\share\\foo");
}

TEST_CASE("trailing punctuation does not derail flagging",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("trail");
    auto flagged = scan_outside_workspace_paths(
        "echo done; type C:\\foo\\bar.txt;", ws);
    REQUIRE(flagged.size() == 1);
    // Trailing ';' is stripped before canonicalization, but the original
    // token (with the punct) is what the user sees in the banner.
    REQUIRE(flagged[0] == "C:\\foo\\bar.txt;");
}

TEST_CASE("bare integer arguments are not treated as paths",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("ints");
    auto flagged = scan_outside_workspace_paths(
        "ping 8.8.8.8 -n 2", ws);
    REQUIRE(flagged.empty());
}

TEST_CASE("no false positive on flag-shaped tokens",
          "[s4.v][path-scanner]")
{
    auto ws = make_temp_ws("flags");
    auto flagged = scan_outside_workspace_paths(
        "cmake --build build/release --config Release", ws);
    REQUIRE(flagged.empty());
}
