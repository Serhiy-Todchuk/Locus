# Manual Test Plan -- ZIM Reader (S6.2)

Locus opens a Kiwix / Wikipedia `.zim` archive directly as a read-only
workspace: articles become virtual "files" that the agent searches and reads
like any other workspace, with no extraction step. These tests use a real ZIM
snapshot -- a small one (e.g. `wikipedia_en_simple_all_nopic_*.zim`, or the
emergency-prep `www.ready.gov_en_*.zim`) keeps the first index fast. Download
one from https://download.kiwix.org/zim/ if you don't have it.

The big English-Wikipedia ZIM (~12 GB, ~21M articles) is a valid target but
its first index takes a long time -- use it only when you specifically want to
test scale.

---

## Test 1 -- Open a ZIM, index it, search and read an article

**Goal.** A `.zim` file opens as a workspace, indexes its articles, and the
agent answers from them with the untrusted-origin marker visible.

1. Launch with a ZIM path: `locus_gui.exe D:\path\to\some.zim` (or use
   File > Open and pick the `.zim` file). The workspace opens; the title bar /
   status reflects the archive.
2. Watch the Activity panel for the index build. For a small ZIM (a few
   thousand articles) expect it to finish in seconds to a couple of minutes;
   the final activity row reports the article count (e.g.
   `ZIM index built: 2083 article(s)`).
3. In chat, ask a question the corpus can answer (for a Wikipedia ZIM:
   "What is the capital of Australia?"; for ready.gov: "What should go in an
   emergency kit?"). The agent should call `search_text` / `read_file` and
   answer from an article.
4. Expand the tool calls in chat (or the Activity panel). The `search_text`
   results must show the `[wikipedia, untrusted]` marker on each surfaced
   snippet, and the `read_file` output header should note the untrusted
   wikipedia source.

**Expected.** Agent answers from real article content, citing an article path,
and every surfaced ZIM snippet carries the untrusted-origin marker.

**Hard fail.** The workspace refuses to open a `.zim` (treats it as "not a
directory"); the index stays at 0 articles; or search returns article text
with NO untrusted marker -- that marker is the S6.0 trust-boundary signal and
must always appear on ZIM content.

---

## Test 2 -- Read-only behaviour and browsing

**Goal.** Confirm the archive is genuinely read-only and that the article
listing is browseable.

1. In the same ZIM workspace, ask the agent to "list the articles" or run a
   `/list_directory` style request. The listing should enumerate article
   paths and note it is a read-only ZIM archive.
2. Ask the agent to create or edit a file (e.g. "write a file notes.txt with
   ... "). The write should not land on disk inside the archive -- a ZIM is
   immutable; the agent has no real folder to write into. (It may report it
   cannot, or the attempt simply has no effect on the archive.)
3. Close Locus and reopen the same `.zim`. The second open should be fast --
   the index built on the first open is reused (it lives in a
   `.locus-zim-<name>` folder next to the `.zim`), not rebuilt from scratch.

**Expected.** Browsing works and is labelled read-only; the archive content is
never modified; reopening reuses the existing index.

---

## Notes

- The per-archive index lives in a `.locus-zim-<archive-name>` folder beside
  the `.zim` file (not inside it -- a ZIM can't be written to). Deleting that
  folder forces a clean re-index on next open.
- Two different `.zim` files in the same folder get separate index folders and
  can be opened independently.
- Prompt-injection scanning over ZIM content is OFF by default (encyclopedia
  text is overwhelmingly benign and a multi-GB scan isn't worth it); the
  `[wikipedia, untrusted]` origin stamp is always present regardless. The scan
  can be enabled with `security.scan_zim` in `.locus/config.json` if desired.
