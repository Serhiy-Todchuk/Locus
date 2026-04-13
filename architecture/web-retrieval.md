# Web Retrieval Subsystem

> RAG over web content. Fetch, index locally, query via existing search tools.
> Full web pages never enter LLM context — only ranked snippets.

---

## Design Principle: Index, Don't Inject

Consumer-grade local LLMs have small context windows (4K–16K tokens).
A single web page can be 5K–50K tokens of raw text. Injecting even one page
would consume the entire context budget.

**Solution**: treat fetched web content like temporary workspace files.
Downloaded pages are stripped to plain text, stored in the index database,
and queried through the same search tools the agent already uses.
The agent sees ranked snippets, not full pages.

```
                        Token cost in LLM context
                        ─────────────────────────
web_search(query)       ~50 tokens (titles + snippets from search API)
web_fetch(url)          ~30 tokens (outline: title, headings, word count)
search_text("term")     ~200 tokens per result snippet (same as local files)
                        ─────────────────────────
Total for one web lookup: ~300 tokens vs 10,000+ for raw page injection
```

---

## Pipeline

```
User message: "How does X work?"
         │
         ▼
    Agent calls web_search("X")
    ┌─────────────────────────────────────────────────┐
    │ Search API (Brave / SearXNG / configurable)     │
    │ Returns: [{title, url, snippet}, ...]           │
    │ Approval: always (user sees query before send)  │
    └─────────────┬───────────────────────────────────┘
                  │  ~50 tokens injected as tool result
                  ▼
    Agent picks a URL, calls web_fetch(url)
    ┌─────────────────────────────────────────────────┐
    │ 1. HTTP GET (cpr)                               │
    │ 2. HTML → plain text (gumbo-parser)             │
    │ 3. Extract: title, headings, text blocks        │
    │ 4. Store in web_pages table                     │
    │ 5. Index text in web_fts (FTS5)                 │
    │ 6. Return to agent: title + headings + stats    │
    │ Approval: always (user sees URL before fetch)   │
    └─────────────┬───────────────────────────────────┘
                  │  ~30 tokens injected (outline only)
                  ▼
    Agent calls search_text("specific term")
    ┌─────────────────────────────────────────────────┐
    │ FTS5 searches BOTH local files AND web_fts      │
    │ Returns ranked snippets with source markers     │
    │ Agent reads only what it needs                  │
    └─────────────────────────────────────────────────┘
```

---

## Storage Schema

Web content lives in **separate tables** from workspace files.
The workspace index is permanent; web content is ephemeral.

```sql
-- Fetched web page metadata
CREATE TABLE web_pages (
    id          INTEGER PRIMARY KEY,
    url         TEXT UNIQUE NOT NULL,
    title       TEXT,
    domain      TEXT,              -- extracted from URL for display
    word_count  INTEGER,
    fetched_at  INTEGER,           -- unix timestamp
    session_id  TEXT               -- scope cleanup to session lifetime
);

-- FTS5 over fetched web page content
CREATE VIRTUAL TABLE web_fts USING fts5(
    url UNINDEXED,
    content,
    tokenize='porter unicode61'
);

-- Web page headings (reuses same structure as local headings)
CREATE TABLE web_headings (
    id          INTEGER PRIMARY KEY,
    page_id     INTEGER REFERENCES web_pages(id),
    level       INTEGER,
    text        TEXT,
    line_number INTEGER
);
CREATE INDEX web_headings_page ON web_headings(page_id);
```

### Why separate tables?

- **Lifecycle**: workspace index persists across sessions. Web cache is ephemeral —
  cleaned up when a session ends or after a configurable TTL.
- **Search filtering**: agent can search "local only" or "local + web" explicitly.
  No risk of stale web results polluting workspace queries.
- **Disk budget**: web cache has a configurable size cap (default 50 MB).
  Oldest pages evicted when the cap is hit.

---

## HTML → Text Extraction

**Library: gumbo-parser** (Google, Apache 2.0, vcpkg: `gumbo`)

- Correct HTML5 parser — handles malformed HTML gracefully
- Pure C, tiny footprint, no dependencies
- Produces a DOM tree; walk it to extract text nodes

Extraction rules:
- **Skip**: `<script>`, `<style>`, `<nav>`, `<footer>`, `<header>`, `<aside>` tags
- **Extract**: visible text nodes from `<body>`
- **Preserve**: heading hierarchy (`<h1>`–`<h6>`) for outline generation
- **Preserve**: paragraph boundaries as line breaks (readable chunking)
- **Strip**: all HTML tags, attributes, inline styles
- **Truncate**: stop after `max_web_page_kb` (default 512 KB of extracted text)

The result is clean, readable plain text — similar quality to browser "reader mode".

---

## Tools

### `web_search`

```
Name:        web_search
Description: Search the web. Returns titles, URLs, and brief snippets.
Params:      query (string, required), max_results (integer, default 5)
Approval:    always
Result:      [{title, url, snippet}, ...] — compact, ~50 tokens total
```

### `web_fetch`

```
Name:        web_fetch
Description: Fetch a web page and index it for searching. Returns page outline.
Params:      url (string, required)
Approval:    always
Result:      {title, url, word_count, headings: [{level, text}, ...]}
             — compact outline, ~30 tokens. Full content indexed, not returned.
```

### `web_read`

```
Name:        web_read
Description: Read a section of a previously fetched web page by heading.
Params:      url (string, required),
             heading (string, optional — read section under this heading),
             offset (integer, optional — line offset),
             length (integer, optional — number of lines)
Approval:    auto
Result:      Text content of the requested section, paginated.
```

After `web_fetch`, the agent has three ways to access web content:
1. `search_text` / `search_hybrid` — finds specific terms across local + web
2. `web_read` — reads a specific section by heading (like `read_file` but for web pages)
3. Both return snippets/sections, never full pages

---

## Search API Backend

Configurable. The user picks their search provider.

### Brave Search API (recommended default)
- Clean JSON REST API, well-documented
- Free tier: 2,000 queries/month, 1 req/sec
- No scraping, no browser automation
- API key stored in `.locus/config.json`

### SearXNG (self-hosted alternative)
- Open source metasearch engine, runs locally
- No API key needed, no rate limits
- User runs their own instance, points Locus at it
- JSON API: `GET /search?q=query&format=json`

### Configuration

```json
{
  "web": {
    "enabled": true,
    "search_provider": "brave",
    "api_key": "BSA...",
    "api_url": "https://api.search.brave.com/res/v1/web/search",
    "max_results": 5,
    "cache_ttl_hours": 24,
    "cache_max_mb": 50,
    "max_web_page_kb": 512
  }
}
```

For custom/self-hosted providers, `api_url` points to any endpoint that returns
results in the expected JSON shape (documented in a response adapter interface).

---

## Cache Management

- **Scope**: web pages are associated with a `session_id`
- **TTL**: pages older than `cache_ttl_hours` are deleted on next startup
- **Size cap**: when `cache_max_mb` is exceeded, oldest pages are evicted
- **Manual clear**: user can clear web cache from settings
- **Session cleanup**: when a session is destroyed, its web pages are eligible for cleanup
  (kept if TTL hasn't expired, in case another session references the same URL)

---

## Config Additions to WorkspaceConfig

```cpp
struct WebConfig {
    bool enabled = false;              // opt-in per workspace
    std::string search_provider = "brave";
    std::string api_key;
    std::string api_url = "https://api.search.brave.com/res/v1/web/search";
    int max_results = 5;
    int cache_ttl_hours = 24;
    int cache_max_mb = 50;
    int max_web_page_kb = 512;
};
```

---

## Security Considerations

- `web_fetch` requires user approval for every URL — no silent fetching
- `web_search` requires user approval for every query — user sees what is sent
- No cookies, no JavaScript execution, no form submission
- User-Agent identifies as Locus (not a browser)
- HTTPS only by default; HTTP allowed only with explicit config flag
- API keys stored in `.locus/config.json` (workspace-local, gitignored)
- Fetched content is local-only — never sent anywhere except back to the local LLM

---

## Open Questions

- Should web content participate in semantic/vector search (S2.1) or FTS5 only?
  Leaning FTS5-only initially — embedding web pages adds CPU cost for ephemeral content.
- Rate limiting: should Locus enforce its own rate limit beyond the API provider's?
  Probably yes — a simple token bucket (1 req/sec default) prevents accidental bursts.
- Robots.txt: should `web_fetch` respect robots.txt? Probably not for single-page fetches
  (we're acting like a user clicking a link, not a crawler), but worth discussing.
