#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "agent/checkpoint_store.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

// Each test uses its own subdir so parallel test invocations don't collide.
fs::path make_temp_root(const std::string& tag)
{
    auto root = fs::temp_directory_path() / ("locus_checkpoint_" + tag);
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("CheckpointStore: snapshot + restore round-trip (edit)", "[s4.b]")
{
    auto root = make_temp_root("roundtrip");
    auto ws_root = root / "ws";
    auto cp_root = root / ".locus" / "checkpoints";
    fs::create_directories(ws_root);

    auto target = ws_root / "hello.txt";
    write_file(target, "hello world\n");

    locus::CheckpointStore store(cp_root);

    REQUIRE(store.snapshot_before("sess1", 1, ws_root, target, "edit"));

    // Mutate the file (simulating an edit_file tool execution).
    write_file(target, "GOODBYE\n");
    REQUIRE(read_file(target) == "GOODBYE\n");

    auto result = store.restore_turn("sess1", 1, ws_root);
    REQUIRE(result.restored == 1);
    REQUIRE(result.deleted  == 0);
    REQUIRE(result.skipped  == 0);
    REQUIRE(result.errors.empty());

    REQUIRE(read_file(target) == "hello world\n");
}

TEST_CASE("CheckpointStore: deleting a created file undoes the create", "[s4.b]")
{
    auto root = make_temp_root("create");
    auto ws_root = root / "ws";
    auto cp_root = root / ".locus" / "checkpoints";
    fs::create_directories(ws_root);

    auto target = ws_root / "new.txt";
    REQUIRE(!fs::exists(target));

    locus::CheckpointStore store(cp_root);

    // Snapshot before a write_file that creates the file.
    REQUIRE(store.snapshot_before("sess1", 1, ws_root, target, "write"));

    // Tool "creates" the file.
    write_file(target, "freshly created\n");
    REQUIRE(fs::exists(target));

    auto result = store.restore_turn("sess1", 1, ws_root);
    REQUIRE(result.restored == 0);
    REQUIRE(result.deleted  == 1);
    REQUIRE(result.skipped  == 0);
    REQUIRE_FALSE(fs::exists(target));
}

TEST_CASE("CheckpointStore: delete-action snapshot restores the file", "[s4.b]")
{
    auto root = make_temp_root("delete");
    auto ws_root = root / "ws";
    auto cp_root = root / ".locus" / "checkpoints";
    fs::create_directories(ws_root);

    auto target = ws_root / "old.txt";
    write_file(target, "important data\n");

    locus::CheckpointStore store(cp_root);
    REQUIRE(store.snapshot_before("sess1", 1, ws_root, target, "delete"));

    fs::remove(target);
    REQUIRE_FALSE(fs::exists(target));

    auto result = store.restore_turn("sess1", 1, ws_root);
    REQUIRE(result.restored == 1);
    REQUIRE(result.deleted  == 0);
    REQUIRE(read_file(target) == "important data\n");
}

TEST_CASE("CheckpointStore: GC keeps last N turns per session", "[s4.b]")
{
    auto root = make_temp_root("gc");
    auto ws_root = root / "ws";
    auto cp_root = root / ".locus" / "checkpoints";
    fs::create_directories(ws_root);

    locus::CheckpointStore store(cp_root);
    store.set_max_turns_per_session(3);

    // Create 5 turns, each touching its own file.
    for (int t = 1; t <= 5; ++t) {
        auto p = ws_root / ("f" + std::to_string(t) + ".txt");
        write_file(p, "v" + std::to_string(t));
        REQUIRE(store.snapshot_before("sess1", t, ws_root, p, "edit"));
    }

    auto turns_before = store.list_turns("sess1");
    REQUIRE(turns_before.size() == 5);

    int pruned = store.gc_session("sess1");
    REQUIRE(pruned == 2);

    auto turns_after = store.list_turns("sess1");
    REQUIRE(turns_after.size() == 3);
    REQUIRE(turns_after.front().turn_id == 3);
    REQUIRE(turns_after.back().turn_id  == 5);
}

TEST_CASE("CheckpointStore: oversized files are recorded but skipped", "[s4.b]")
{
    auto root = make_temp_root("size");
    auto ws_root = root / "ws";
    auto cp_root = root / ".locus" / "checkpoints";
    fs::create_directories(ws_root);

    auto target = ws_root / "big.bin";
    std::string blob(2048, 'X');
    write_file(target, blob);

    locus::CheckpointStore store(cp_root);
    store.set_max_file_size_bytes(1024);  // 1 KB cap, file is 2 KB

    REQUIRE(store.snapshot_before("sess1", 1, ws_root, target, "edit"));

    auto info = store.read_turn("sess1", 1);
    REQUIRE(info.has_value());
    REQUIRE(info->entries.size() == 1);
    REQUIRE(info->entries[0].skipped);
    REQUIRE(info->entries[0].existed);

    // Mutate the file.
    write_file(target, "tiny");

    auto result = store.restore_turn("sess1", 1, ws_root);
    REQUIRE(result.restored == 0);
    REQUIRE(result.skipped  == 1);
    REQUIRE_FALSE(result.errors.empty());

    // Mutation persists because the snapshot was skipped.
    REQUIRE(read_file(target) == "tiny");
}

TEST_CASE("CheckpointStore: re-snapshotting same path in a turn is a no-op", "[s4.b]")
{
    auto root = make_temp_root("idempotent");
    auto ws_root = root / "ws";
    auto cp_root = root / ".locus" / "checkpoints";
    fs::create_directories(ws_root);

    auto target = ws_root / "f.txt";
    write_file(target, "ORIGINAL");

    locus::CheckpointStore store(cp_root);
    REQUIRE(store.snapshot_before("sess1", 1, ws_root, target, "edit"));

    // First mutation.
    write_file(target, "MUTATED");

    // A second snapshot for the same path within the same turn must NOT
    // overwrite the original snapshot — otherwise undo would restore the
    // already-mutated state.
    REQUIRE(store.snapshot_before("sess1", 1, ws_root, target, "edit"));

    write_file(target, "MUTATED2");

    auto result = store.restore_turn("sess1", 1, ws_root);
    REQUIRE(result.restored == 1);
    REQUIRE(read_file(target) == "ORIGINAL");
}

TEST_CASE("CheckpointStore: drop_turn / drop_session", "[s4.b]")
{
    auto root = make_temp_root("drop");
    auto ws_root = root / "ws";
    auto cp_root = root / ".locus" / "checkpoints";
    fs::create_directories(ws_root);

    auto target = ws_root / "f.txt";
    write_file(target, "x");

    locus::CheckpointStore store(cp_root);
    store.snapshot_before("sess1", 1, ws_root, target, "edit");
    store.snapshot_before("sess1", 2, ws_root, target, "edit");

    REQUIRE(store.list_turns("sess1").size() == 2);
    REQUIRE(store.drop_turn("sess1", 1));
    REQUIRE(store.list_turns("sess1").size() == 1);

    REQUIRE(store.drop_session("sess1"));
    REQUIRE(store.list_turns("sess1").empty());
}

TEST_CASE("CheckpointStore: empty session_id disables snapshotting", "[s4.b]")
{
    auto root = make_temp_root("empty");
    auto ws_root = root / "ws";
    auto cp_root = root / ".locus" / "checkpoints";
    fs::create_directories(ws_root);

    auto target = ws_root / "f.txt";
    write_file(target, "x");

    locus::CheckpointStore store(cp_root);
    REQUIRE_FALSE(store.snapshot_before("", 1, ws_root, target, "edit"));
    REQUIRE(store.list_turns("").empty());
}
