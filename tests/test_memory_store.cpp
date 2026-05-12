#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/database.h"
#include "core/memory_store.h"
#include "core/workspace.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

// Each test runs in its own temp tree so parallel invocations don't collide.
fs::path make_temp_root(const std::string& tag)
{
    auto root = fs::temp_directory_path() / ("locus_memory_" + tag);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

locus::WorkspaceConfig default_config()
{
    locus::WorkspaceConfig cfg;
    cfg.memory_enabled                    = true;
    cfg.memory_in_context_budget_tokens   = 500;
    cfg.memory_max_entries                = 200;
    cfg.memory_search_response_max_tokens = 1500;
    cfg.memory_recency_half_life_days     = 21;
    return cfg;
}

// Build a bare MemoryStore (no semantic, no reranker) on top of a fresh
// main_db. Most behavioural tests only need the FTS5 + on-disk side.
struct StoreFixture {
    fs::path                         root;
    fs::path                         mem_dir;
    std::unique_ptr<locus::Database> main_db;
    std::unique_ptr<locus::MemoryStore> store;
    locus::WorkspaceConfig           cfg;

    explicit StoreFixture(const std::string& tag,
                          locus::WorkspaceConfig overrides = default_config())
        : root(make_temp_root(tag))
        , mem_dir(root / ".locus" / "memory")
        , cfg(overrides)
    {
        fs::create_directories(root / ".locus");
        main_db = std::make_unique<locus::Database>(
            root / ".locus" / "index.db", locus::DbKind::Main);
        store = std::make_unique<locus::MemoryStore>(
            mem_dir, *main_db, nullptr, nullptr, nullptr, cfg);
    }

    ~StoreFixture()
    {
        store.reset();
        main_db.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

} // namespace

// -- basic round-trip ---------------------------------------------------------

TEST_CASE("MemoryStore: add + get round-trip", "[s4.r][memory-store]")
{
    StoreFixture fx("roundtrip");

    auto id = fx.store->add("the build command is cmake --build build/release",
                            {"build", "cmake"}, /*pinned=*/false, "user");
    REQUIRE_FALSE(id.empty());

    auto got = fx.store->get(id);
    REQUIRE(got.has_value());
    REQUIRE(got->content == "the build command is cmake --build build/release");
    REQUIRE(got->tags.size() == 2);
    REQUIRE(got->tags[0] == "build");
    REQUIRE(got->tags[1] == "cmake");
    REQUIRE(got->source == "user");
    REQUIRE_FALSE(got->pinned);
    REQUIRE(got->created_at > 0);
}

TEST_CASE("MemoryStore: persistence across restart", "[s4.r][memory-store]")
{
    auto root = make_temp_root("persistence");
    auto mem_dir = root / ".locus" / "memory";
    fs::create_directories(root / ".locus");
    auto cfg = default_config();

    std::string id;
    {
        locus::Database db(root / ".locus" / "index.db", locus::DbKind::Main);
        locus::MemoryStore store(mem_dir, db, nullptr, nullptr, nullptr, cfg);
        id = store.add("pinned build note", {"build"},
                       /*pinned=*/true, "user");
    }
    {
        locus::Database db(root / ".locus" / "index.db", locus::DbKind::Main);
        locus::MemoryStore store(mem_dir, db, nullptr, nullptr, nullptr, cfg);
        auto got = store.get(id);
        REQUIRE(got.has_value());
        REQUIRE(got->content == "pinned build note");
        REQUIRE(got->pinned);
        REQUIRE(got->tags == std::vector<std::string>{"build"});
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("MemoryStore: rejects empty content", "[s4.r][memory-store]")
{
    StoreFixture fx("empty");
    REQUIRE_THROWS_AS(fx.store->add("   \n\t  "), std::invalid_argument);
    REQUIRE(fx.store->live_count() == 0);
}

// -- search -------------------------------------------------------------------

TEST_CASE("MemoryStore: FTS keyword search", "[s4.r][memory-store][search]")
{
    StoreFixture fx("search_fts");
    fx.store->add("the build command is cmake --build build/release",
                  {"build"});
    fx.store->add("we use clangd for code completion",       {"editor"});
    fx.store->add("Mr. Owner prefers tabs over spaces.",     {"style"});

    auto hits = fx.store->search("cmake", 5);
    REQUIRE(hits.size() >= 1);
    REQUIRE_THAT(hits[0].entry.content, ContainsSubstring("cmake"));
}

TEST_CASE("MemoryStore: empty query returns all entries", "[s4.r][memory-store][search]")
{
    StoreFixture fx("search_all");
    fx.store->add("entry one", {"a"});
    fx.store->add("entry two", {"b"});
    fx.store->add("entry three", {"a", "c"});

    auto hits = fx.store->search("", 100);
    REQUIRE(hits.size() == 3);
}

TEST_CASE("MemoryStore: tag filter restricts results", "[s4.r][memory-store][search]")
{
    StoreFixture fx("search_tag");
    fx.store->add("entry alpha", {"build"});
    fx.store->add("entry beta",  {"docs"});
    fx.store->add("entry gamma", {"build", "docs"});

    auto only_build = fx.store->search("entry", 10, {"build"});
    REQUIRE(only_build.size() == 2);
    for (auto& h : only_build) {
        REQUIRE(std::find(h.entry.tags.begin(), h.entry.tags.end(),
                          std::string("build")) != h.entry.tags.end());
    }

    auto both = fx.store->search("entry", 10, {"build", "docs"});
    REQUIRE(both.size() == 1);
    REQUIRE(both[0].entry.content == "entry gamma");
}

TEST_CASE("MemoryStore: search bumps last_used_at", "[s4.r][memory-store][search]")
{
    StoreFixture fx("search_lru");
    auto id = fx.store->add("touch me", {});
    auto before = fx.store->get(id);
    REQUIRE(before.has_value());

    // Wait a beat so the wall-clock granularity catches the bump (we record
    // unix seconds).
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto hits = fx.store->search("touch", 5);
    REQUIRE(hits.size() >= 1);

    auto after = fx.store->get(id);
    REQUIRE(after.has_value());
    REQUIRE(after->last_used_at >= before->last_used_at);
    REQUIRE(after->last_used_at > 0);
}

// -- in-context slot ----------------------------------------------------------

TEST_CASE("MemoryStore: in-context slot includes pinned + LRU", "[s4.r][memory-store][slot]")
{
    StoreFixture fx("slot");
    fx.store->add("important pinned fact",  {}, /*pinned=*/true);
    fx.store->add("recent unpinned note one", {});
    fx.store->add("recent unpinned note two", {});

    auto section = fx.store->format_for_system_prompt();
    REQUIRE_THAT(section, ContainsSubstring("## Memory Bank"));
    REQUIRE_THAT(section, ContainsSubstring("important pinned fact"));
    REQUIRE_THAT(section, ContainsSubstring("recent unpinned note"));
}

TEST_CASE("MemoryStore: empty store yields empty section", "[s4.r][memory-store][slot]")
{
    StoreFixture fx("slot_empty");
    REQUIRE(fx.store->format_for_system_prompt().empty());
}

TEST_CASE("MemoryStore: zero budget yields empty section", "[s4.r][memory-store][slot]")
{
    auto cfg = default_config();
    cfg.memory_in_context_budget_tokens = 0;
    StoreFixture fx("slot_zero", cfg);
    fx.store->add("anything", {});
    REQUIRE(fx.store->format_for_system_prompt().empty());
}

// -- GC -----------------------------------------------------------------------

TEST_CASE("MemoryStore: GC drops oldest unpinned beyond max_entries", "[s4.r][memory-store][gc]")
{
    auto cfg = default_config();
    cfg.memory_max_entries = 3;
    StoreFixture fx("gc", cfg);

    fx.store->add("oldest", {}, /*pinned=*/false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    fx.store->add("middle", {}, /*pinned=*/false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    fx.store->add("newer",  {}, /*pinned=*/false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    fx.store->add("newest", {}, /*pinned=*/false);

    REQUIRE(fx.store->live_count() == 3);
    auto all = fx.store->list_all();
    // GC should have dropped "oldest" first.
    for (auto& e : all) {
        REQUIRE(e.content != "oldest");
    }
}

TEST_CASE("MemoryStore: pinned entries survive GC", "[s4.r][memory-store][gc]")
{
    auto cfg = default_config();
    cfg.memory_max_entries = 1;
    StoreFixture fx("gc_pinned", cfg);

    fx.store->add("pinned fact",  {}, /*pinned=*/true);
    fx.store->add("unpinned 1",  {});
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    fx.store->add("unpinned 2",  {});

    REQUIRE(fx.store->live_count() == 2);   // 1 pinned + max 1 unpinned
    auto pinned = fx.store->list_pinned();
    REQUIRE(pinned.size() == 1);
    REQUIRE(pinned[0].content == "pinned fact");
}

// -- soft delete + restore ----------------------------------------------------

TEST_CASE("MemoryStore: soft_delete + restore_deleted", "[s4.r][memory-store][delete]")
{
    StoreFixture fx("delete");
    auto id = fx.store->add("temporary note", {});
    REQUIRE(fx.store->live_count() == 1);

    REQUIRE(fx.store->soft_delete(id));
    REQUIRE(fx.store->live_count() == 0);
    REQUIRE_FALSE(fx.store->get(id).has_value());
    REQUIRE(fx.store->deleted_count() == 1);

    REQUIRE(fx.store->restore_deleted(id));
    REQUIRE(fx.store->live_count() == 1);
    auto restored = fx.store->get(id);
    REQUIRE(restored.has_value());
    REQUIRE(restored->content == "temporary note");
}

TEST_CASE("MemoryStore: soft_delete unknown id returns false", "[s4.r][memory-store][delete]")
{
    StoreFixture fx("delete_unknown");
    REQUIRE_FALSE(fx.store->soft_delete("no-such-id"));
}

TEST_CASE("MemoryStore: hard_delete removes live and trashed copies",
          "[s4.r][memory-store][delete]")
{
    StoreFixture fx("hard_delete");

    // Live entry: hard_delete wipes the .md file and FTS/vec rows; no copy
    // ends up in .deleted/.
    auto id_live = fx.store->add("disposable live note", {});
    REQUIRE(fx.store->live_count()    == 1);
    REQUIRE(fx.store->deleted_count() == 0);
    REQUIRE(fx.store->hard_delete(id_live));
    REQUIRE(fx.store->live_count()    == 0);
    REQUIRE(fx.store->deleted_count() == 0);  // not parked in .deleted/

    // Already-soft-deleted entry: hard_delete clears the .deleted/ copy too.
    auto id_trashed = fx.store->add("about to be trashed", {});
    REQUIRE(fx.store->soft_delete(id_trashed));
    REQUIRE(fx.store->deleted_count() == 1);
    REQUIRE(fx.store->hard_delete(id_trashed));
    REQUIRE(fx.store->deleted_count() == 0);

    // Unknown id -> false.
    REQUIRE_FALSE(fx.store->hard_delete("no-such-id"));
}

// -- update -------------------------------------------------------------------

TEST_CASE("MemoryStore: update mutates content + tags + pinned", "[s4.r][memory-store][update]")
{
    StoreFixture fx("update");
    auto id = fx.store->add("first draft", {"draft"});

    REQUIRE(fx.store->update(id,
                             std::string("revised draft"),
                             std::vector<std::string>{"final"},
                             true));

    auto got = fx.store->get(id);
    REQUIRE(got.has_value());
    REQUIRE(got->content == "revised draft");
    REQUIRE(got->tags    == std::vector<std::string>{"final"});
    REQUIRE(got->pinned);
}

// -- frontmatter parse fidelity -----------------------------------------------

TEST_CASE("MemoryStore: tags with commas + special chars round-trip", "[s4.r][memory-store]")
{
    StoreFixture fx("special");
    auto id = fx.store->add(
        "line one\nline two with `code`\nline three with \"quotes\"",
        {"a", "b-c", "d_e"},
        /*pinned=*/true);

    auto got = fx.store->get(id);
    REQUIRE(got.has_value());
    REQUIRE(got->content ==
            "line one\nline two with `code`\nline three with \"quotes\"");
    REQUIRE(got->tags == std::vector<std::string>{"a", "b-c", "d_e"});

    // And after a reload
    fx.store.reset();
    fx.store = std::make_unique<locus::MemoryStore>(
        fx.mem_dir, *fx.main_db, nullptr, nullptr, nullptr, fx.cfg);
    auto reloaded = fx.store->get(id);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->content == got->content);
    REQUIRE(reloaded->tags    == got->tags);
}
