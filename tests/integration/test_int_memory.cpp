// S4.R -- memory bank.
//
// Verifies the three user-observable surfaces added in this stage:
//   1. /memorize slash command (parses +tag tokens and --pin flag, writes
//      directly through MemoryStore, bypasses approval).
//   2. add_memory tool (LLM-driven; approval policy `ask` -- the harness
//      auto-approves).
//   3. search_memory tool (LLM-driven; auto-approve; hybrid FTS5 + cosine
//      + recency).
//
// Tests use unique marker strings per case so they don't interfere with one
// another or with any prior memory entries the user has in WS1. Cleanup
// removes only the entries each test added.

#include "harness_fixture.h"

#include "core/memory_store.h"
#include "core/workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <random>
#include <set>
#include <string>

using namespace locus::integration;

namespace {

// Per-process random suffix so concurrent test runs don't clash.
std::string mint_marker(const std::string& tag)
{
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 0xFFFFFF);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%06x", dist(rng));
    return "S4R-INT-" + tag + "-" + buf;
}

// Snapshot ids currently in the memory bank so a test can identify and clean
// up only the entries it added.
std::set<std::string> snapshot_ids(locus::MemoryStore& mem)
{
    std::set<std::string> out;
    for (auto& e : mem.list_all()) out.insert(e.id);
    return out;
}

void cleanup_added_since(locus::MemoryStore& mem,
                         const std::set<std::string>& before)
{
    // hard_delete (not soft_delete) so test runs don't pile garbage into the
    // workspace's `.locus/memory/.deleted/` retention bin.
    for (auto& e : mem.list_all()) {
        if (before.find(e.id) == before.end())
            mem.hard_delete(e.id);
    }
}

} // namespace

// -- /memorize slash command (no LLM round-trip required) ---------------------

TEST_CASE("/memorize stores a verbatim entry with a tag and pin flag",
          "[integration][memory]")
{
    auto& h = harness();
    auto* mem = h.workspace().memory();
    REQUIRE(mem != nullptr);

    auto before = snapshot_ids(*mem);
    const auto marker = mint_marker("slash-pin");

    // /memorize bypasses the approval gate; the response arrives via on_token.
    PromptResult r = h.prompt("/memorize +build --pin " + marker
                              + " is the canonical marker.");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_calls.empty());           // slash dispatches no tools
    INFO("/memorize output:\n" << r.tokens);
    REQUIRE(r.tokens.find("Saved memory:") != std::string::npos);
    REQUIRE(r.tokens.find("(pinned)")      != std::string::npos);
    REQUIRE(r.tokens.find("tags=build")    != std::string::npos);

    // Engine side: a new entry shows up with our marker, the pin flag set,
    // and the tag we asked for.
    bool found = false;
    for (auto& e : mem->list_all()) {
        if (before.count(e.id)) continue;
        REQUIRE(e.pinned);
        REQUIRE(e.tags == std::vector<std::string>{"build"});
        REQUIRE(e.content.find(marker) != std::string::npos);
        REQUIRE(e.source == "user");
        found = true;
    }
    REQUIRE(found);

    cleanup_added_since(*mem, before);
}

TEST_CASE("/memorize accepts plain content without tags or flags",
          "[integration][memory]")
{
    auto& h = harness();
    auto* mem = h.workspace().memory();
    REQUIRE(mem != nullptr);

    auto before = snapshot_ids(*mem);
    const auto marker = mint_marker("slash-plain");

    PromptResult r = h.prompt("/memorize " + marker + " has no tags.");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_calls.empty());
    INFO("/memorize output:\n" << r.tokens);
    REQUIRE(r.tokens.find("Saved memory:") != std::string::npos);
    // No pin, no tag => those decorations are absent.
    REQUIRE(r.tokens.find("(pinned)") == std::string::npos);
    REQUIRE(r.tokens.find("tags=")    == std::string::npos);

    int added = 0;
    for (auto& e : mem->list_all()) {
        if (before.count(e.id)) continue;
        REQUIRE_FALSE(e.pinned);
        REQUIRE(e.tags.empty());
        REQUIRE(e.content.find(marker) != std::string::npos);
        ++added;
    }
    REQUIRE(added == 1);

    cleanup_added_since(*mem, before);
}

// -- /forget slash command (user-only delete; no LLM round-trip) -------------

TEST_CASE("/forget --hard removes an entry permanently", "[integration][memory]")
{
    auto& h = harness();
    auto* mem = h.workspace().memory();
    REQUIRE(mem != nullptr);

    auto before = snapshot_ids(*mem);
    const auto marker = mint_marker("forget-hard");

    // Seed an entry via the store so we know the exact id.
    auto id = mem->add(marker + " disposable note", {}, false, "user");
    REQUIRE_FALSE(id.empty());
    REQUIRE(mem->get(id).has_value());

    PromptResult r = h.prompt("/forget " + id + " --hard");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_calls.empty());
    INFO("/forget output:\n" << r.tokens);
    REQUIRE(r.tokens.find("Forgot memory:") != std::string::npos);
    REQUIRE(r.tokens.find("(permanently)") != std::string::npos);

    // hard_delete must also wipe the .deleted/ copy -- nothing is left under
    // either directory for this id.
    REQUIRE_FALSE(mem->get(id).has_value());

    cleanup_added_since(*mem, before);  // no-op now, but cheap
}

TEST_CASE("/forget on an unknown id reports an error", "[integration][memory]")
{
    auto& h = harness();
    auto* mem = h.workspace().memory();
    REQUIRE(mem != nullptr);

    PromptResult r = h.prompt("/forget definitely-not-a-real-id");
    REQUIRE_FALSE(r.timed_out);
    // The dispatcher routes the "no such id" message through on_error.
    INFO("/forget tokens: " << r.tokens
         << "\nerrors: " << (r.errors.empty() ? "<none>" : r.errors.front()));
    bool reported = false;
    for (auto& e : r.errors)
        if (e.find("no memory entry") != std::string::npos) reported = true;
    REQUIRE(reported);
}

// -- add_memory tool (LLM-driven; approval `ask` -- harness auto-approves) ----

TEST_CASE("add_memory tool commits an entry the LLM chose to save",
          "[integration][llm][memory]")
{
    auto& h = harness();
    auto* mem = h.workspace().memory();
    REQUIRE(mem != nullptr);

    auto before = snapshot_ids(*mem);
    const auto marker = mint_marker("tool-add");

    // Phrasing nudges a tool-trained model toward picking add_memory. The
    // exact tags/pin flags are deliberately the model's choice -- we only
    // assert that the entry landed.
    PromptResult r = h.prompt(
        "Use the add_memory tool exactly once to save the following note "
        "verbatim, then reply with the single word DONE.\n"
        "Note to save: \"" + marker + " is the canonical S4.R int-test marker.\"");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("add_memory"));

    bool found = false;
    for (auto& e : mem->list_all()) {
        if (before.count(e.id)) continue;
        if (e.content.find(marker) != std::string::npos) {
            // Tool path sets source=agent when the agent invokes it (the
            // default in AddMemoryTool::execute when "source" isn't passed).
            REQUIRE(e.source == "agent");
            found = true;
        }
    }
    REQUIRE(found);

    cleanup_added_since(*mem, before);
}

// -- search_memory tool (LLM-driven; auto-approve) ----------------------------

TEST_CASE("search_memory tool retrieves a pre-seeded entry",
          "[integration][llm][memory]")
{
    auto& h = harness();
    auto* mem = h.workspace().memory();
    REQUIRE(mem != nullptr);

    auto before = snapshot_ids(*mem);
    const auto marker = mint_marker("tool-search");

    // Seed an entry directly via the store so we don't depend on the model
    // calling add_memory first.
    const std::string note =
        marker + ": the bge-m3 embedder runs at LLAMA_POOLING_TYPE_LAST.";
    auto seeded_id = mem->add(note, {"embedder"}, /*pinned=*/false, "user");
    REQUIRE_FALSE(seeded_id.empty());

    // Ask the LLM to retrieve it. The unique marker means a hit is
    // unambiguous; the model is free to phrase its summary however it likes.
    PromptResult r = h.prompt(
        "Use the search_memory tool to look up notes about the marker "
        "string `" + marker + "`. Then in your reply quote the marker "
        "exactly so I know you found it.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("search_memory"));

    // The tool's result content (which the LLM saw) must include the marker
    // somewhere -- that's the unambiguous "the hit reached the model" signal.
    auto* res = r.find_result_for("search_memory");
    REQUIRE(res != nullptr);
    INFO("search_memory display:\n" << res->display);
    REQUIRE(res->display.find(marker) != std::string::npos);

    cleanup_added_since(*mem, before);
}
