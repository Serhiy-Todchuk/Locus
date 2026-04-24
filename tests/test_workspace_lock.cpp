#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/workspace_lock.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path fresh_dir(const std::string& name)
{
    auto p = fs::temp_directory_path() / "locus_test_lock" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

} // namespace

TEST_CASE("WorkspaceLock: first acquirer succeeds", "[s4.a][lock]")
{
    auto ws = fresh_dir("basic");
    REQUIRE_NOTHROW(locus::WorkspaceLock{ws});
    REQUIRE(fs::exists(ws / ".locus" / "locus.lock"));
    fs::remove_all(ws);
}

TEST_CASE("WorkspaceLock: second lock on same workspace fails", "[s4.a][lock]")
{
    auto ws = fresh_dir("same_path");
    {
        locus::WorkspaceLock first{ws};
        REQUIRE_THROWS_WITH(locus::WorkspaceLock{ws},
                            ContainsSubstring("already open"));
    }
    fs::remove_all(ws);
}

TEST_CASE("WorkspaceLock: releases on destruction — next acquirer succeeds", "[s4.a][lock]")
{
    auto ws = fresh_dir("release");
    {
        locus::WorkspaceLock first{ws};
        // first falls out of scope here — OS releases the lock.
    }
    REQUIRE_NOTHROW(locus::WorkspaceLock{ws});
    fs::remove_all(ws);
}

TEST_CASE("WorkspaceLock: child folder refuses when parent workspace is open",
          "[s4.a][lock]")
{
    auto parent = fresh_dir("nested_parent");
    fs::create_directories(parent / "src");

    {
        locus::WorkspaceLock parent_lock{parent};

        // Opening `parent/src` should fail because the ancestor is locked.
        REQUIRE_THROWS_WITH(locus::WorkspaceLock{parent / "src"},
                            ContainsSubstring("ancestor"));
    }
    fs::remove_all(parent);
}

TEST_CASE("WorkspaceLock: child folder succeeds once parent is closed",
          "[s4.a][lock]")
{
    auto parent = fresh_dir("nested_after_close");
    fs::create_directories(parent / "src");

    {
        locus::WorkspaceLock parent_lock{parent};
        // parent lock falls out of scope
    }

    {
        locus::WorkspaceLock child{parent / "src"};
    }
    fs::remove_all(parent);
}

TEST_CASE("WorkspaceLock: writes diagnostic JSON with PID", "[s4.a][lock]")
{
    // Windows' mandatory byte-range lock blocks concurrent reads within the
    // locked range, so we acquire + release + *then* read the persisted file.
    auto ws = fresh_dir("diagnostic");
    {
        locus::WorkspaceLock lock{ws};
    }
    std::string content;
    {
        std::ifstream in(ws / ".locus" / "locus.lock");
        content.assign((std::istreambuf_iterator<char>(in)), {});
    }
    REQUIRE_THAT(content, ContainsSubstring("\"pid\""));
    REQUIRE_THAT(content, ContainsSubstring("\"started\""));

    fs::remove_all(ws);
}
