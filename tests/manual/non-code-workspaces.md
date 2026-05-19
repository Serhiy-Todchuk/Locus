# Manual Test Plan -- Non-Code Workspaces (S5.N)

Locus claims to serve document folders and offline references just as well
as code repositories. This plan turns that claim into reproducible QA on two
demo workspaces:

- **WS2** -- offline Simple English Wikipedia subset, built by
  [scripts/build_ws2.ps1](../../scripts/build_ws2.ps1).
- **WS3** -- mixed personal-documents library (IETF RFCs as PDF,
  Apache POI sample DOCX/XLSX, hand-authored Markdown notes), built by
  [scripts/build_ws3.ps1](../../scripts/build_ws3.ps1).

Both scripts default to building under `D:\Projects\LocusTestWorkspaces\`.
S5.N's acceptance bar is **8 of the 10 acceptance prompts return useful,
sourced answers** without manually pasting file contents -- the three tests
below collectively run all 10.

**Pre-S6.2 caveat.** WS2 here is the pre-extracted form of a small ZIM,
not native libzim access. [S6.2](../../roadmap/M6/S6.2-zim-reader.md) is
the long-term path; S5.N proves the workflow on what we already have.

---

## Test 1 -- WS2 (offline Wikipedia subset)

**Goal.** Prove that semantic + keyword retrieval works on a moderately
large HTML-only corpus, and that the agent cites the right article files.

1. From the repo root run `pwsh ./scripts/build_ws2.ps1`. First run
   downloads ~250 MB ZIM + kiwix-tools and extracts 5000 articles
   (configurable via `-MaxArticles`). Expect ~5-10 minutes.
2. Launch `locus_gui.exe D:\Projects\LocusTestWorkspaces\WS2_Wikipedia`.
   Workspace Capabilities modal appears. Confirm defaults are sensible
   for this corpus (semantic search on, memory bank on, code-aware
   search off, background processes off, web off) -- accept and proceed.
3. Wait for first index + embedding queue to drain (watch the Activity
   panel; queue completion is visible as the `embedding queue: drained`
   activity row). Expect ~1-3 minutes on the recommended profile.
4. Run **prompts 1-4** in fresh chats (one per prompt, `/reset` between):
   - "What is the capital of Australia?" -> cites `A/Australia.html`
     or `A/Canberra.html` in the activity log; answer text is correct.
   - "Compare TCP and UDP at a high level." -> draws on multiple article
     files (the underlying Simple English coverage of these topics).
     Hard fail: if the model hallucinates without any `search_*` /
     `read_file` activity log entry, retrieval isn't being used.
   - "Who came up with the theory of evolution by natural selection?"
     -> cites `A/Charles_Darwin.html`; answer "Charles Darwin".
     (Earlier drafts used paraphrased queries -- "species change over
     time", "Byzantine fault tolerance" -- but those either named
     uncovered topics or pushed past the small-profile embedder's
     paraphrase ceiling. See perf notes in `test-workspaces.md`.)
   - "What does this corpus say about quantum chromodynamics?" -> the
     model says it cannot find that topic. Hard fail: a fabricated
     summary with no citation means the read-only "answer not present"
     framing in `LOCUS.md` isn't being heeded.

**Expected.** 3 of 4 prompts produce sourced answers. Activity panel for
each successful prompt shows at least one `search_text` / `search_semantic`
/ `search_hybrid` call followed by a `read_file` on a hit. The negative
prompt explicitly admits no coverage.

---

## Test 2 -- WS3 (mixed personal-documents library)

**Goal.** Exercise PDF / DOCX / XLSX / Markdown extractors end-to-end,
prove cross-format retrieval (same RFC available as both PDF and TXT),
and verify the read-only policy from `LOCUS.md` is honoured.

1. Run `pwsh ./scripts/build_ws3.ps1` (small, < 1 minute).
2. Launch `locus_gui.exe D:\Projects\LocusTestWorkspaces\WS3_Documents`.
   Accept Capabilities defaults (semantic + memory on, code/bg/web off).
3. Wait for index + embedding queue drain. RFC PDFs go through PDFium;
   DOCX/XLSX through miniz + pugixml. The Activity panel surfaces any
   extractor failure as a warn-level row -- a single warning on a
   PDF/A or encrypted variant is expected behaviour, not a test fail.
4. Run **prompts 5-9** in fresh chats:
   - "Which RFC defines HTTP/3?" -> cites `rfcs/rfc9114.txt` or
     `.pdf`. Both should rank near the top; either is acceptable.
   - "Give me an outline of RFC 7540." -> agent calls
     `get_file_outline rfcs/rfc7540.pdf` and renders the section
     structure. Hard fail: if it `read_file`s the whole PDF without
     an outline call, the PDF heading extractor isn't being trusted.
   - "Summarise my notes about web protocols." -> draws from
     `notes/web-protocols.md`, optionally cross-references the HTTP/2 +
     HTTP/3 RFCs.
   - "Find the cell content in samples/SampleSS.xlsx." -> XLSX
     extractor surfaces text; agent reports what it found. (If the
     POI sample URL rotted and the file is missing, this is a "skip,
     not fail" condition -- noted in the build script log.)
   - "What does my notes folder say about TLS 1.3, and which RFC is
     it from?" -> answer combines `notes/security.md` (notes prose)
     with `rfcs/rfc8446.pdf` (canonical source). Cross-format + cross-
     document synthesis in one prompt.
   - "Edit notes/networking.md to add a paragraph about IPSec." ->
     agent **refuses** the edit per `LOCUS.md` ("read-only personal
     docs"). Hard fail: an `edit_file` approval pane appearing here
     means the LOCUS.md framing isn't reaching the system prompt.

**Expected.** 5 of 6 prompts produce sourced answers; the edit prompt is
declined in prose. Inspect `D:\Projects\LocusTestWorkspaces\WS3_Documents\.locus\locus.log`
if anything is unclear -- search for `tool_call` rows.

---

## Test 3 -- Retrieval quality smoke

**Goal.** Score the indexes built in Tests 1 + 2 with the gold-query
sets so a future retrieval refactor has a sanity floor to compare against.
Numbers vary by model profile; record them in the run output, not in this
plan.

1. Build the eval harness if not already present:
   `cmake --build build/release --config Release --target locus_retrieval_eval`.
2. Run against WS2:
   ```
   build/release/tests/retrieval_eval/Release/locus_retrieval_eval.exe \
     --workspace D:/Projects/LocusTestWorkspaces/WS2_Wikipedia \
     --queries   tests/retrieval_eval/queries_ws2.json \
     --out       tests/retrieval_eval/results_ws2.md \
     --baseline  tests/retrieval_eval/baseline_ws2_none.json
   ```
3. Run against WS3:
   ```
   build/release/tests/retrieval_eval/Release/locus_retrieval_eval.exe \
     --workspace D:/Projects/LocusTestWorkspaces/WS3_Documents \
     --queries   tests/retrieval_eval/queries_ws3.json \
     --out       tests/retrieval_eval/results_ws3.md \
     --baseline  tests/retrieval_eval/baseline_ws3_none.json
   ```
   The `--baseline` paths are intentionally nonexistent so the harness skips
   the WS1-baseline comparison; the workspace-specific baseline is not
   committed (see retrieval_eval/README.md).
4. Embedding the corpus is slow on CPU (bge-m3 ~5 chunks/sec). The
   harness waits up to 600 s for the queue to drain; on the WS2 5000-
   article corpus that's not enough to fully cover, and the first run
   exits with a "proceeding with partial coverage" warning. Re-run the
   same command -- vectors.db persists, so the second run resumes from
   where the queue stopped and the recall numbers stabilise once
   `Embedding queue drained: 0/0` appears in the log.
5. Open both `results_*.md`. Reference numbers committed alongside this
   plan:
   - **WS2** (bge-small profile, 5000 articles, full drain): all three
     methods 14/14 at recall@5; semantic best at MRR=1.000.
   - **WS3** (bge-m3 profile, PDF-only RFCs + Markdown notes): semantic
     and hybrid 13/13 at recall@5; text 6/13 (RFC formal language
     doesn't align with natural-language queries).
   - Pattern to expect: search_hybrid ~ search_semantic > search_text.
     Pure-keyword running below 50% hit-rate on a non-code corpus is
     normal and means semantic is earning its keep, not that something
     is broken.
   - Below ~75% hit-rate on hybrid means re-check extraction first
     (open a hit's `read_file` output, confirm HTMLExtractor / PDFium
     produced usable text). The S5.N investigation surfaced two
     extraction/indexing bugs this way: see
     [test-workspaces.md](../../test-workspaces.md) "Bugs surfaced by
     the S5.N investigation" for the details.

Numbers below those bands are not a fail -- they're a signal to inspect
extraction (open a few hits' `read_file` output to confirm PDFium /
HTMLExtractor produced usable text) before blaming retrieval.

**No baselines are committed for WS2/WS3.** Workspaces aren't on every
dev's disk; baselines stay WS1-only. If a retrieval refactor in M6 needs
a regression guard for non-code corpora, reactivate.

---

## Honest gaps (do not file as bugs)

- **WS2 is pre-extracted, not live ZIM.** Native `zim::Archive` access,
  full ~21M-article scale, and namespace browsing all land with
  [S6.2](../../roadmap/M6/S6.2-zim-reader.md). The current path proves
  the indexing + retrieval workflow on a smaller flat-folder corpus.
- **WS3 size is intentionally tiny** (~10-20 MB). It exists to exercise
  every extractor path on one machine quickly, not to be representative
  of a real document hoard. Swap in your own folder if you want scale.
- **Embedding wall time scales with corpus.** 5000 Wikipedia articles
  on the small-profile (`bge-small`) embedder is sub-minute; on the
  recommended profile (`bge-m3`) it's a few minutes. The full 250K
  Simple English ZIM at `-MaxArticles 0` is closer to an hour on
  recommended -- run overnight or stay on the default cap.
