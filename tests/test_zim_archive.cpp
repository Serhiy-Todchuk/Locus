// S6.2 -- ZIM archive parser tests. Drive the MIT reader against a real test
// ZIM when one is present (D:/Projects/WikiZimTest/*.zim on the dev box), and
// SKIP gracefully when it is absent so the suite stays hermetic on machines
// without the multi-GB corpus. The reader itself is exercised end-to-end:
// header decode, article enumeration, cluster decompression, content read.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "zim/zim_archive.h"

#include "core/workspace.h"
#include "index/index_query.h"
#include "tools/file_tools.h"
#include "tools/index_tools.h"
#include "tools/search_tools.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

// Resolve a test ZIM, in priority order:
//   1. $LOCUS_TEST_ZIM    -- explicit override (point at any archive)
//   2. tests/sample_docs/sample.zim -- the 40 KB synthetic fixture COMMITTED to
//      the repo (openZIM testing-suite `nons/small.zim`), so the ZIM tests run
//      hermetically everywhere with no multi-GB dependency.
//   3. D:/Projects/WikiZimTest/*.zim -- the dev box's large real corpora
//      (ready.gov / Wikipedia), smallest first, for a richer-content smoke.
// Returns {} only if none are present (tests then WARN + skip).
fs::path find_test_zim()
{
    std::error_code ec;
    if (const char* env = std::getenv("LOCUS_TEST_ZIM")) {
        fs::path p(env);
        if (fs::exists(p, ec)) return p;
    }

    // Committed fixture -- walk up from CWD to the repo's tests/sample_docs.
    {
        fs::path base = fs::current_path();
        for (int i = 0; i < 6; ++i) {
            fs::path candidate = base / "tests" / "sample_docs" / "sample.zim";
            if (fs::exists(candidate, ec)) return candidate;
            if (!base.has_parent_path() || base.parent_path() == base) break;
            base = base.parent_path();
        }
    }

    const fs::path dir = "D:/Projects/WikiZimTest";
    if (fs::is_directory(dir, ec)) {
        fs::path best;
        uintmax_t best_sz = 0;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() == ".zim") {
                uintmax_t sz = e.file_size(ec);
                if (best.empty() || sz < best_sz) { best = e.path(); best_sz = sz; }
            }
        }
        if (!best.empty()) return best;
    }
    return {};
}

} // namespace

TEST_CASE("ZimArchive: open + enumerate + read a real test ZIM", "[s6.2][zim]")
{
    fs::path zim = find_test_zim();
    if (zim.empty()) {
        WARN("No test ZIM found (set LOCUS_TEST_ZIM or place one in "
             "D:/Projects/WikiZimTest); skipping ZIM archive test.");
        return;
    }

    locus::zim::ZimArchive ar(zim);

    SECTION("header sanity") {
        REQUIRE(ar.major_version() >= 5);
        REQUIRE(ar.entry_count() > 0);
        REQUIRE(ar.cluster_count() > 0);
    }

    SECTION("article enumeration is non-empty and well-formed") {
        const auto& arts = ar.articles();
        REQUIRE(!arts.empty());
        // Every enumerated article must carry a path; title falls back to path.
        for (size_t i = 0; i < std::min<size_t>(arts.size(), 50); ++i) {
            REQUIRE(!arts[i].path.empty());
            REQUIRE(!arts[i].title.empty());
        }
    }

    SECTION("read the first article -> non-empty HTML-ish content") {
        const auto& arts = ar.articles();
        REQUIRE(!arts.empty());
        auto content = ar.read_content(arts.front().entry_index);
        REQUIRE(content.has_value());
        REQUIRE(!content->empty());
    }

    SECTION("read by path round-trips") {
        const auto& arts = ar.articles();
        REQUIRE(!arts.empty());
        // Pick an article a few entries in (the very first can be a special
        // landing page); read it back by its namespace-stripped path.
        const auto& pick = arts[std::min<size_t>(3, arts.size() - 1)];
        auto by_path = ar.read_article(pick.path);
        REQUIRE(by_path.has_value());
        REQUIRE(!by_path->empty());
    }

    SECTION("decompression yields readable text for several articles") {
        // Sample a spread of articles to exercise multiple clusters /
        // compression. At least most should decode to non-empty bodies.
        const auto& arts = ar.articles();
        size_t ok = 0, tried = 0;
        size_t step = std::max<size_t>(1, arts.size() / 20);
        for (size_t i = 0; i < arts.size() && tried < 20; i += step, ++tried) {
            auto c = ar.read_content(arts[i].entry_index);
            if (c && !c->empty()) ++ok;
        }
        REQUIRE(tried > 0);
        REQUIRE(ok >= tried / 2);  // tolerate the odd asset/redirect edge case
    }
}

TEST_CASE("ZimWorkspace: open archive, index, search, read with origin stamp",
          "[s6.2][zim][workspace]")
{
    fs::path zim = find_test_zim();
    if (zim.empty()) {
        WARN("No test ZIM found; skipping ZIM workspace integration test.");
        return;
    }

    // Open the .zim as a workspace -- this builds the per-archive index by
    // bulk-feeding every article through Indexer::index_virtual_article.
    locus::Workspace ws(zim);
    REQUIRE(ws.zim_mode());
    REQUIRE(ws.zim_archive() != nullptr);

    auto* idx = ws.index();
    REQUIRE(idx != nullptr);

    SECTION("articles landed in the index") {
        auto rows = idx->list_directory("", 1'000'000);
        size_t files = 0;
        for (const auto& fe : rows) if (!fe.is_directory) ++files;
        REQUIRE(files > 0);
    }

    SECTION("search_text surfaces an article with the untrusted origin marker") {
        // Derive a query word from an actual indexed article so the test works
        // on ANY ZIM corpus (the 40 KB synthetic small.zim or a content-rich
        // Wikipedia dump) rather than guessing vocabulary. Read the first
        // indexed article and pull a reasonably long alphabetic token from it.
        auto rows = idx->list_directory("", 1'000'000);
        std::string a_path;
        for (const auto& fe : rows)
            if (!fe.is_directory) { a_path = fe.path; break; }
        REQUIRE(!a_path.empty());

        auto body = idx->get_extracted_text(a_path);
        REQUIRE(body.has_value());

        std::string term;
        {
            std::string cur;
            for (char c : *body) {
                if (std::isalpha(static_cast<unsigned char>(c))) cur += c;
                else { if (cur.size() >= 4) { term = cur; break; } cur.clear(); }
            }
            if (term.empty() && cur.size() >= 4) term = cur;
        }
        REQUIRE(!term.empty());  // every real article has at least one 4+ letter word

        locus::SearchTextTool tool;
        locus::ToolCall call;
        call.tool_name = "search_text";
        call.args = nlohmann::json::object();
        call.args["query"] = term;

        auto res = tool.execute(call, ws);
        REQUIRE(res.success);
        REQUIRE(res.content.find("0 results") == std::string::npos);
        // Every surfaced snippet from a ZIM row carries the [wikipedia,
        // untrusted] origin marker (S6.0 Task D taint surface).
        REQUIRE_THAT(res.content, ContainsSubstring("[wikipedia, untrusted]"));
    }

    SECTION("read_file serves stripped article text for a virtual path") {
        const auto& arts = ws.zim_archive()->articles();
        REQUIRE(!arts.empty());
        // Pick an article whose path made it into the index (some entries may
        // strip to empty and be skipped). Find one via list_directory.
        auto rows = idx->list_directory("", 1'000'000);
        std::string a_path;
        for (const auto& fe : rows) {
            if (!fe.is_directory) { a_path = fe.path; break; }
        }
        REQUIRE(!a_path.empty());

        locus::ReadFileTool tool;
        locus::ToolCall call;
        call.tool_name = "read_file";
        call.args = nlohmann::json::object();
        call.args["path"] = a_path;
        auto res = tool.execute(call, ws);
        REQUIRE(res.success);
        REQUIRE(!res.content.empty());
        // The header notes the untrusted wikipedia source.
        REQUIRE_THAT(res.content, ContainsSubstring("untrusted"));
    }

    SECTION("list_directory enumerates virtual articles read-only") {
        locus::ListDirectoryTool tool;
        locus::ToolCall call;
        call.tool_name = "list_directory";
        call.args = nlohmann::json::object();
        call.args["max_entries"] = 20;
        auto res = tool.execute(call, ws);
        REQUIRE(res.success);
        REQUIRE_THAT(res.content, ContainsSubstring("ZIM archive, read-only"));
    }
}

TEST_CASE("ZimArchive: bad magic is rejected", "[s6.2][zim]")
{
    // Write a tiny non-ZIM file and confirm the reader throws ZimError.
    fs::path tmp = fs::temp_directory_path() / "locus_not_a.zim";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "this is definitely not a zim file, padding padding padding "
             "padding padding padding padding padding padding padding ok";
    }
    REQUIRE_THROWS_AS(locus::zim::ZimArchive(tmp), locus::zim::ZimError);
    std::error_code ec;
    fs::remove(tmp, ec);
}
