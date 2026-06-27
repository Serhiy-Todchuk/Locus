// S6.1 -- Web Retrieval (RAG).
//
// Hermetic coverage: HTML extraction quality (gumbo drops chrome subtrees),
// the ephemeral web cache (store / read / outline / TTL + size eviction),
// web_fts search integration via IndexQuery::search_text(sources="web"), and
// the search-provider JSON adapters. No network round-trips here -- the live
// web_search / web_fetch path is exercised manually / in the integration suite.

#include <catch2/catch_test_macros.hpp>

#include "core/database.h"
#include "core/workspace_config.h"
#include "core/workspace_config_json.h"
#include "extractors/html_extractor.h"
#include "index/index_query.h"
#include "web/web_cache.h"
#include "web/web_search_provider.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace locus;

namespace {

fs::path make_db_dir(const char* suffix)
{
    auto tmp = fs::temp_directory_path() / ("locus_web_test_" + std::string(suffix));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    return tmp;
}

} // namespace

// -- HTML extraction (gumbo) --------------------------------------------------

TEST_CASE("HtmlExtractor drops chrome subtrees and keeps article text",
          "[s6.1][web][html]")
{
    const std::string html =
        "<html><head><title>T</title>"
        "<style>body{color:red}</style></head>"
        "<body>"
        "<header>Site banner SHOULD_NOT_APPEAR</header>"
        "<nav>Menu LINK_SHOULD_NOT_APPEAR</nav>"
        "<aside>Sidebar ADVERT_SHOULD_NOT_APPEAR</aside>"
        "<main><h1>Real Heading</h1>"
        "<p>Keep this body sentence.</p>"
        "<h2>Sub Section</h2>"
        "<p>Second paragraph kept.</p></main>"
        "<footer>Copyright FOOTER_SHOULD_NOT_APPEAR</footer>"
        "<script>var x = 1; SCRIPT_SHOULD_NOT_APPEAR;</script>"
        "</body></html>";

    auto r = HtmlExtractor::extract_from_string(html);

    REQUIRE_FALSE(r.is_binary);
    // Body text survives.
    REQUIRE(r.text.find("Keep this body sentence.") != std::string::npos);
    REQUIRE(r.text.find("Second paragraph kept.") != std::string::npos);
    // Chrome is dropped.
    REQUIRE(r.text.find("SHOULD_NOT_APPEAR") == std::string::npos);
    REQUIRE(r.text.find("color:red") == std::string::npos);
    // No raw tags leak through.
    REQUIRE(r.text.find("<p>") == std::string::npos);

    // Headings captured in order with levels.
    REQUIRE(r.headings.size() == 2);
    REQUIRE(r.headings[0].level == 1);
    REQUIRE(r.headings[0].text == "Real Heading");
    REQUIRE(r.headings[1].level == 2);
    REQUIRE(r.headings[1].text == "Sub Section");
}

TEST_CASE("HtmlExtractor handles malformed HTML without crashing",
          "[s6.1][web][html]")
{
    // Unclosed tags, stray entities -- gumbo's error recovery should still
    // produce text rather than throw.
    const std::string html =
        "<html><body><h1>Broken<p>paragraph without close "
        "&amp; an entity <b>bold and <i>nested";
    auto r = HtmlExtractor::extract_from_string(html);
    REQUIRE_FALSE(r.is_binary);
    REQUIRE(r.text.find("paragraph without close") != std::string::npos);
    REQUIRE(r.text.find("&amp;") == std::string::npos);  // entity decoded
    REQUIRE(r.text.find("&") != std::string::npos);      // decoded to literal &
}

// -- config round-trip --------------------------------------------------------

TEST_CASE("WebConfig round-trips through JSON", "[s6.1][web][config]")
{
    WorkspaceConfig cfg;
    cfg.web.enabled         = true;
    cfg.web.search_provider = "searxng";
    cfg.web.api_key         = "secret-key";
    cfg.web.api_url         = "http://localhost:8888/search";
    cfg.web.max_results     = 9;
    cfg.web.cache_ttl_hours = 6;
    cfg.web.cache_max_mb    = 100;
    cfg.web.max_web_page_kb = 256;
    cfg.web.allow_http      = true;

    auto j = workspace_config_to_json(cfg);
    auto back = workspace_config_from_json(j);

    REQUIRE(back.web.enabled);
    REQUIRE(back.web.search_provider == "searxng");
    REQUIRE(back.web.api_key == "secret-key");
    REQUIRE(back.web.api_url == "http://localhost:8888/search");
    REQUIRE(back.web.max_results == 9);
    REQUIRE(back.web.cache_ttl_hours == 6);
    REQUIRE(back.web.cache_max_mb == 100);
    REQUIRE(back.web.max_web_page_kb == 256);
    REQUIRE(back.web.allow_http);

    // Defaults survive an empty document (no "web" key).
    auto fresh = workspace_config_from_json(nlohmann::json::object());
    REQUIRE_FALSE(fresh.web.enabled);
    REQUIRE(fresh.web.search_provider == "brave");
}

// -- extract_domain -----------------------------------------------------------

TEST_CASE("extract_domain pulls the host", "[s6.1][web][domain]")
{
    REQUIRE(extract_domain("https://en.wikipedia.org/wiki/Cat") == "en.wikipedia.org");
    REQUIRE(extract_domain("http://example.com:8080/path?q=1") == "example.com");
    REQUIRE(extract_domain("https://user:pw@host.test/x") == "host.test");
    REQUIRE(extract_domain("https://plain.example") == "plain.example");
    // Unrecognised shape returns the input unchanged.
    REQUIRE(extract_domain("not a url") == "not a url");
}

// -- WebCache store / read / outline ------------------------------------------

TEST_CASE("WebCache stores and reads back a page", "[s6.1][web][cache]")
{
    auto dir = make_db_dir("store");
    {
        Database db(dir / "index.db", DbKind::Main);
        WebCache cache(db);

        std::vector<ExtractedHeading> headings = {
            {1, "Intro", 1},
            {2, "Details", 5},
        };
        int64_t id = cache.store_page(
            "https://example.com/doc",
            "Example Doc",
            "Intro\nfirst body line\n\nDetails\nsecond body line\n",
            headings, "sess1", /*injection_flags=*/0);
        REQUIRE(id > 0);
        REQUIRE(cache.page_count() == 1);
        REQUIRE(cache.has_page("https://example.com/doc"));
        REQUIRE_FALSE(cache.has_page("https://example.com/other"));

        auto content = cache.get_content("https://example.com/doc");
        REQUIRE(content.has_value());
        REQUIRE(content->find("first body line") != std::string::npos);

        REQUIRE(cache.get_title("https://example.com/doc") == "Example Doc");

        auto h = cache.get_headings("https://example.com/doc");
        REQUIRE(h.size() == 2);
        REQUIRE(h[0].text == "Intro");
        REQUIRE(h[1].text == "Details");
    }
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("WebCache re-fetch replaces the prior row", "[s6.1][web][cache]")
{
    auto dir = make_db_dir("refetch");
    {
        Database db(dir / "index.db", DbKind::Main);
        WebCache cache(db);
        cache.store_page("https://x.test/a", "Old", "old content body", {}, "s", 0);
        cache.store_page("https://x.test/a", "New", "new content body here", {}, "s", 0);
        REQUIRE(cache.page_count() == 1);  // upsert, not append
        auto c = cache.get_content("https://x.test/a");
        REQUIRE(c.has_value());
        REQUIRE(c->find("new content") != std::string::npos);
        REQUIRE(c->find("old content") == std::string::npos);
        REQUIRE(cache.get_title("https://x.test/a") == "New");
    }
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("WebCache TTL and size eviction", "[s6.1][web][cache][eviction]")
{
    auto dir = make_db_dir("evict");
    {
        Database db(dir / "index.db", DbKind::Main);
        WebCache cache(db);

        // Three pages with progressively larger bodies.
        cache.store_page("https://e.test/1", "P1", std::string(2000, 'a'), {}, "s", 0);
        cache.store_page("https://e.test/2", "P2", std::string(2000, 'b'), {}, "s", 0);
        cache.store_page("https://e.test/3", "P3", std::string(2000, 'c'), {}, "s", 0);
        REQUIRE(cache.page_count() == 3);
        REQUIRE(cache.total_content_bytes() >= 6000);

        // TTL=0 disables; nothing evicted.
        REQUIRE(cache.evict_expired(0) == 0);
        REQUIRE(cache.page_count() == 3);

        // Far-future TTL keeps everything (pages just stored).
        REQUIRE(cache.evict_expired(24) == 0);
        REQUIRE(cache.page_count() == 3);

        // Size cap below 3 pages: oldest evicted until under budget.
        int removed = cache.evict_over_size(4500);
        REQUIRE(removed >= 1);
        REQUIRE(cache.total_content_bytes() <= 4500);
        // The newest page must survive.
        REQUIRE(cache.has_page("https://e.test/3"));

        cache.clear();
        REQUIRE(cache.page_count() == 0);
        REQUIRE(cache.total_content_bytes() == 0);
    }
    std::error_code ec; fs::remove_all(dir, ec);
}

// -- web_fts search via IndexQuery --------------------------------------------

TEST_CASE("search_text includes web pages when sources include web",
          "[s6.1][web][search]")
{
    auto dir = make_db_dir("search");
    {
        Database main_db(dir / "index.db", DbKind::Main);
        WebCache cache(main_db);
        cache.store_page("https://w.test/sqlite",
                         "SQLite FTS5",
                         "The bm25 function ranks full text search results.",
                         {}, "s", /*injection_flags=*/0);

        IndexQuery q(main_db, /*vectors_db=*/nullptr);

        // Default (files-only) finds nothing -- the workspace index is empty.
        SearchOptions files_only;
        auto none = q.search_text("bm25", files_only);
        REQUIRE(none.empty());

        // Web-only finds the cached page, origin-stamped.
        SearchOptions web;
        web.include_files = false;
        web.include_web   = true;
        auto hits = q.search_text("bm25", web);
        REQUIRE(hits.size() == 1);
        REQUIRE(hits[0].path == "https://w.test/sqlite");
        REQUIRE(hits[0].origin == "web");
        REQUIRE(hits[0].snippet.find("bm25") != std::string::npos);
    }
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("search_text web source is a clean no-op without a web cache",
          "[s6.1][web][search]")
{
    auto dir = make_db_dir("nocache");
    {
        Database main_db(dir / "index.db", DbKind::Main);
        // No WebCache constructed -> web_fts table never created.
        IndexQuery q(main_db, nullptr);
        SearchOptions web;
        web.include_files = false;
        web.include_web   = true;
        auto hits = q.search_text("anything", web);
        REQUIRE(hits.empty());  // probe short-circuits, no SQL error
    }
    std::error_code ec; fs::remove_all(dir, ec);
}

// -- search-provider JSON adapters --------------------------------------------

TEST_CASE("parse_brave_results extracts hits", "[s6.1][web][provider]")
{
    const std::string body = R"({
        "web": { "results": [
            {"title": "First", "url": "https://a.test/1", "description": "snip one"},
            {"title": "Second", "url": "https://b.test/2", "description": "snip two"},
            {"title": "NoUrl"}
        ]}
    })";
    auto hits = parse_brave_results(body, 5);
    REQUIRE(hits.size() == 2);  // the no-url row is dropped
    REQUIRE(hits[0].title == "First");
    REQUIRE(hits[0].url == "https://a.test/1");
    REQUIRE(hits[0].snippet == "snip one");

    // max_results cap honoured.
    auto one = parse_brave_results(body, 1);
    REQUIRE(one.size() == 1);

    // Garbage / missing shape -> empty, no throw.
    REQUIRE(parse_brave_results("not json", 5).empty());
    REQUIRE(parse_brave_results("{}", 5).empty());
}

TEST_CASE("parse_searxng_results extracts hits", "[s6.1][web][provider]")
{
    const std::string body = R"({
        "results": [
            {"title": "X", "url": "https://x.test", "content": "body x"}
        ]
    })";
    auto hits = parse_searxng_results(body, 5);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].url == "https://x.test");
    REQUIRE(hits[0].snippet == "body x");

    REQUIRE(parse_searxng_results("[]", 5).empty());
}

TEST_CASE("web_search_query validates before hitting the network",
          "[s6.1][web][provider]")
{
    WebSearchSettings s;
    s.provider = "brave";
    s.api_url  = "https://api.search.brave.com/res/v1/web/search";
    // No api_key -> fails fast (Brave requires one) with a useful message.
    auto r = web_search_query("cats", s);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("api_key") != std::string::npos);

    // http URL with allow_http off -> refused.
    WebSearchSettings s2;
    s2.provider = "searxng";
    s2.api_url  = "http://localhost:8888/search";
    s2.allow_http = false;
    auto r2 = web_search_query("cats", s2);
    REQUIRE_FALSE(r2.ok);
    REQUIRE(r2.error.find("http") != std::string::npos);
}
