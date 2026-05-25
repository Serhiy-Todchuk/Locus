#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/database.h"
#include "core/memory_filter.h"
#include "core/memory_store.h"
#include "core/workspace.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path make_temp_root(const std::string& tag)
{
    auto root = fs::temp_directory_path() / ("locus_mbf_" + tag);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

locus::WorkspaceConfig default_config()
{
    locus::WorkspaceConfig cfg;
    cfg.memory.enabled                    = true;
    cfg.memory.in_context_budget_tokens   = 500;
    cfg.memory.max_entries                = 200;
    cfg.memory.search_response_max_tokens = 1500;
    cfg.memory.recency_half_life_days     = 21;
    return cfg;
}

struct StoreFixture {
    fs::path                                root;
    fs::path                                mem_dir;
    std::unique_ptr<locus::Database>        main_db;
    std::unique_ptr<locus::MemoryStore>     store;
    locus::WorkspaceConfig                  cfg;

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

    locus::MemoryStore::Entry make_entry(const std::string& content,
                                         std::vector<std::string> tags,
                                         std::string source = "agent",
                                         bool pinned = false)
    {
        auto id = store->add(content, tags, pinned, source);
        return *store->get(id);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// apply_filter -- pure
// ---------------------------------------------------------------------------

TEST_CASE("MemoryFilter: empty filter returns all entries", "[s5.k][memory-filter]")
{
    StoreFixture fx("empty");
    fx.make_entry("first",  {"a"});
    fx.make_entry("second", {"b"});
    fx.make_entry("third",  {});

    auto all = fx.store->list_all();
    locus::MemoryFilter f;
    auto out = locus::apply_filter(all, f);
    REQUIRE(out.size() == 3);
}

TEST_CASE("MemoryFilter: query substring matches content/tags/id", "[s5.k][memory-filter]")
{
    StoreFixture fx("query");
    fx.make_entry("Cmake build command", {"build"});
    fx.make_entry("clangd config",       {"editor"});
    fx.make_entry("ninja invocation",    {"build"});

    auto all = fx.store->list_all();

    locus::MemoryFilter f;
    f.query = "cmake";
    auto out = locus::apply_filter(all, f);
    REQUIRE(out.size() == 1);
    REQUIRE_THAT(out[0].content, ContainsSubstring("Cmake"));

    f.query = "build";   // matches both content and tag
    out = locus::apply_filter(all, f);
    REQUIRE(out.size() >= 1);
}

TEST_CASE("MemoryFilter: tag filter is intersection", "[s5.k][memory-filter]")
{
    StoreFixture fx("tags");
    fx.make_entry("alpha", {"build"});
    fx.make_entry("beta",  {"docs"});
    fx.make_entry("gamma", {"build", "docs"});

    auto all = fx.store->list_all();

    locus::MemoryFilter f;
    f.tags = {"build"};
    auto out = locus::apply_filter(all, f);
    REQUIRE(out.size() == 2);

    f.tags = {"build", "docs"};
    out = locus::apply_filter(all, f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].content == "gamma");
}

TEST_CASE("MemoryFilter: source dropdown restriction", "[s5.k][memory-filter]")
{
    StoreFixture fx("source");
    fx.make_entry("u",  {}, "user");
    fx.make_entry("a1", {}, "agent");
    fx.make_entry("a2", {}, "agent");

    auto all = fx.store->list_all();

    locus::MemoryFilter f;
    f.source = "user";
    auto out = locus::apply_filter(all, f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].content == "u");

    f.source = "agent";
    out = locus::apply_filter(all, f);
    REQUIRE(out.size() == 2);

    f.source = "garbage";   // unknown source string -> ignored
    out = locus::apply_filter(all, f);
    REQUIRE(out.size() == 3);
}

TEST_CASE("MemoryFilter: pinned_only drops unpinned", "[s5.k][memory-filter]")
{
    StoreFixture fx("pinned");
    fx.make_entry("pinned-1", {}, "user", /*pinned*/true);
    fx.make_entry("loose-1",  {}, "user", /*pinned*/false);
    fx.make_entry("loose-2",  {}, "agent",/*pinned*/false);

    auto all = fx.store->list_all();
    locus::MemoryFilter f;
    f.pinned_only = true;
    auto out = locus::apply_filter(all, f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].content == "pinned-1");
}

TEST_CASE("MemoryFilter: created_at date range", "[s5.k][memory-filter]")
{
    StoreFixture fx("daterange");
    auto e1 = fx.make_entry("first",  {});
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto e2 = fx.make_entry("second", {});

    auto all = fx.store->list_all();
    // Pick a cutoff between e1 and e2.
    auto cutoff = e1.created_at + 1;
    locus::MemoryFilter f;
    f.created_from = cutoff;
    auto out = locus::apply_filter(all, f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].content == "second");

    locus::MemoryFilter g;
    g.created_to = cutoff - 1;
    out = locus::apply_filter(all, g);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].content == "first");
}

// ---------------------------------------------------------------------------
// sort_entries
// ---------------------------------------------------------------------------

TEST_CASE("sort_entries: pinned_first_last_used puts pinned at top", "[s5.k][memory-filter]")
{
    StoreFixture fx("sort_pinned");
    fx.make_entry("oldest unpinned", {});
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    fx.make_entry("pinned fact",  {}, "user", /*pinned*/true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    fx.make_entry("newest unpinned", {});

    auto all = fx.store->list_all();
    locus::sort_entries(all, locus::MemorySort::pinned_first_last_used);
    REQUIRE(all.front().pinned);
    REQUIRE(all.front().content == "pinned fact");
}

TEST_CASE("sort_entries: content asc/desc are inverses", "[s5.k][memory-filter]")
{
    StoreFixture fx("sort_content");
    fx.make_entry("b-second", {});
    fx.make_entry("a-first",  {});
    fx.make_entry("c-third",  {});

    auto all = fx.store->list_all();
    locus::sort_entries(all, locus::MemorySort::content_asc);
    REQUIRE(all.front().content == "a-first");
    REQUIRE(all.back().content  == "c-third");
    locus::sort_entries(all, locus::MemorySort::content_desc);
    REQUIRE(all.front().content == "c-third");
    REQUIRE(all.back().content  == "a-first");
}

// ---------------------------------------------------------------------------
// collect_unique_tags
// ---------------------------------------------------------------------------

TEST_CASE("collect_unique_tags: returns sorted unique tag set", "[s5.k][memory-filter]")
{
    StoreFixture fx("tags_unique");
    fx.make_entry("e1", {"build", "cmake"});
    fx.make_entry("e2", {"cmake", "test"});
    fx.make_entry("e3", {"build"});

    auto all = fx.store->list_all();
    auto tags = locus::collect_unique_tags(all);
    REQUIRE(tags.size() == 3);
    REQUIRE(tags[0] == "build");
    REQUIRE(tags[1] == "cmake");
    REQUIRE(tags[2] == "test");
}

// ---------------------------------------------------------------------------
// list_deleted_stubs + recovery flow
// ---------------------------------------------------------------------------

TEST_CASE("list_deleted_stubs: empty when nothing deleted", "[s5.k][memory-filter][recovery]")
{
    StoreFixture fx("deleted_empty");
    fx.make_entry("alive", {});
    auto stubs = locus::list_deleted_stubs(*fx.store);
    REQUIRE(stubs.empty());
}

TEST_CASE("list_deleted_stubs: surfaces soft-deleted entries", "[s5.k][memory-filter][recovery]")
{
    StoreFixture fx("deleted_walk");
    auto e1 = fx.make_entry("trashed one", {"a"});
    auto e2 = fx.make_entry("trashed two", {"b", "c"});
    auto e3 = fx.make_entry("kept",        {});

    REQUIRE(fx.store->soft_delete(e1.id));
    REQUIRE(fx.store->soft_delete(e2.id));

    auto stubs = locus::list_deleted_stubs(*fx.store);
    REQUIRE(stubs.size() == 2);
    // We don't assert ordering by id (the sort is by deleted_at_proxy
    // which on systems with 1s resolution may collide for two soft-deletes
    // happening in the same second). Assert set membership instead.
    std::set<std::string> ids;
    for (auto& s : stubs) ids.insert(s.id);
    REQUIRE(ids.count(e1.id) == 1);
    REQUIRE(ids.count(e2.id) == 1);
    REQUIRE(ids.count(e3.id) == 0);
}

TEST_CASE("Recovery: restore_deleted brings entry back into the live store",
          "[s5.k][memory-filter][recovery]")
{
    StoreFixture fx("restore");
    auto e = fx.make_entry("recoverable", {"a"});
    REQUIRE(fx.store->soft_delete(e.id));
    REQUIRE(fx.store->live_count()    == 0);
    REQUIRE(fx.store->deleted_count() == 1);

    REQUIRE(fx.store->restore_deleted(e.id));
    REQUIRE(fx.store->live_count()    == 1);
    REQUIRE(fx.store->deleted_count() == 0);
    auto got = fx.store->get(e.id);
    REQUIRE(got.has_value());
    REQUIRE(got->content == "recoverable");
    REQUIRE(got->tags == std::vector<std::string>{"a"});
}

// ---------------------------------------------------------------------------
// Bulk-ops (exercised through MemoryStore APIs the panel calls)
// ---------------------------------------------------------------------------

TEST_CASE("Bulk delete soft-deletes the selected subset", "[s5.k][memory-filter][bulk]")
{
    StoreFixture fx("bulk_delete");
    auto e1 = fx.make_entry("one", {});
    auto e2 = fx.make_entry("two", {});
    auto e3 = fx.make_entry("three", {});

    REQUIRE(fx.store->soft_delete(e1.id));
    REQUIRE(fx.store->soft_delete(e2.id));

    REQUIRE(fx.store->live_count()    == 1);
    REQUIRE(fx.store->deleted_count() == 2);
    REQUIRE(fx.store->get(e3.id).has_value());
}

TEST_CASE("Bulk tag adds tags and preserves existing tags", "[s5.k][memory-filter][bulk]")
{
    StoreFixture fx("bulk_tag");
    auto e1 = fx.make_entry("a", {"foo"});
    auto e2 = fx.make_entry("b", {});

    // Simulate the panel's tag-merge step.
    auto add_tags = [&](const locus::MemoryStore::Entry& e,
                        const std::vector<std::string>& add) {
        std::set<std::string> merged(e.tags.begin(), e.tags.end());
        for (auto& t : add) merged.insert(t);
        std::vector<std::string> v(merged.begin(), merged.end());
        fx.store->update(e.id, std::nullopt, std::move(v), std::nullopt);
    };
    add_tags(e1, {"new", "bar"});
    add_tags(e2, {"new"});

    auto got1 = fx.store->get(e1.id);
    REQUIRE(got1.has_value());
    REQUIRE(std::find(got1->tags.begin(), got1->tags.end(), "foo") != got1->tags.end());
    REQUIRE(std::find(got1->tags.begin(), got1->tags.end(), "bar") != got1->tags.end());
    REQUIRE(std::find(got1->tags.begin(), got1->tags.end(), "new") != got1->tags.end());

    auto got2 = fx.store->get(e2.id);
    REQUIRE(got2.has_value());
    REQUIRE(got2->tags == std::vector<std::string>{"new"});
}

TEST_CASE("Bulk pin toggles state across selection", "[s5.k][memory-filter][bulk]")
{
    StoreFixture fx("bulk_pin");
    auto e1 = fx.make_entry("pin me 1", {});
    auto e2 = fx.make_entry("pin me 2", {});

    fx.store->update(e1.id, std::nullopt, std::nullopt, true);
    fx.store->update(e2.id, std::nullopt, std::nullopt, true);

    auto pinned = fx.store->list_pinned();
    REQUIRE(pinned.size() == 2);

    fx.store->update(e1.id, std::nullopt, std::nullopt, false);
    fx.store->update(e2.id, std::nullopt, std::nullopt, false);
    REQUIRE(fx.store->list_pinned().empty());
}
