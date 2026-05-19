# Retrieval Evaluation Harness (S4.K)

Manual-only harness that measures retrieval quality on a real workspace. Lets
us tell whether a chunker tweak, embedder swap, or RRF parameter change
actually helps -- instead of shipping retrieval on vibes.

## What it measures

For each gold query in [queries.json](queries.json), runs `search_text`,
`search_semantic`, and `search_hybrid` and computes:

- **recall@K** for K ∈ {1, 3, 5, 10} -- fraction of expected files retrieved in
  the top K
- **MRR** -- mean reciprocal rank of the first hit
- **nDCG@10** -- discounted cumulative gain (binary relevance)
- **wall time** -- total time spent in each method's calls

Granularity is file-level (one path per relevant document). Symbol-level
gold is intentionally not modelled here -- `search_symbols` is a different beast
and is covered by unit tests.

## Running

The binary lives next to the integration tests:

```
cmake --build build/release --target locus_retrieval_eval
./build/release/tests/retrieval_eval/Release/locus_retrieval_eval.exe \
    --workspace . \
    --queries tests/retrieval_eval/queries.json \
    --out tests/retrieval_eval/results.md
```

It will:
1. Open the workspace (full index build runs synchronously in the ctor).
2. Wait for the embedding queue to drain (capped at 7200s = 2 h -- raised
   from the original 600s in [S5.N](../../roadmap/M5/S5.N-non-code-workspace-proof.md)
   because WS2's 5000-article corpus produces ~34k chunks and bge-small
   needs ~55 min to drain on CPU. The 30-second-no-progress short-circuit
   inside `wait_for_embeddings` still triggers for stuck workers, so the
   cap doesn't affect WS1-scale runs in practice). Set `--no-wait` to skip.
3. Run every query through all three methods.
4. Write a markdown report to `--out` and print a summary to stdout.
5. If `tests/retrieval_eval/baseline.json` exists, compare the run against it
   and exit non-zero if any metric dropped more than `--tolerance` (default
   5%).

Exit codes: `0` pass, `1` regression, `2` harness error.

## Establishing / refreshing the baseline

After a deliberate retrieval improvement, commit the new numbers as the
baseline:

```
locus_retrieval_eval --workspace . --write-baseline
```

This overwrites `baseline.json`. Commit it alongside the change that produced
the improvement.

## Authoring queries

Each entry in [queries.json](queries.json) is `{ query, expected_files[] }`.
Keep `expected_files` tight -- 1-3 paths per query. A query that lists 10
"related" files inflates recall and washes out signal. If a real codebase
question genuinely needs 5+ files, split it into multiple queries.

Paths use forward slashes, relative to the workspace root.

## Curating queries from real sessions

The `--curate-from <sessions_dir>` mode walks `.locus/sessions/*.json`,
extracts every `search` / `search_text` / `search_semantic` / `search_hybrid`
tool invocation, and prints a JSON template with empty `expected_files`
arrays:

```
locus_retrieval_eval --curate-from .locus/sessions > new_queries.json
```

Hand-fill `expected_files` and merge into `queries.json`. This keeps the gold
set anchored on questions a real user actually asked.

## Non-code workspaces (S5.N)

Two extra query files measure retrieval on the non-code demo workspaces:

- [queries_ws2.json](queries_ws2.json) -- Simple English Wikipedia subset
  (HTML-only corpus built by [scripts/build_ws2.ps1](../../scripts/build_ws2.ps1)).
- [queries_ws3.json](queries_ws3.json) -- mixed personal-documents library
  (RFCs as PDF + TXT, Apache POI samples, hand-authored Markdown notes,
  built by [scripts/build_ws3.ps1](../../scripts/build_ws3.ps1)).

Run them with the same harness, swapping `--workspace` + `--queries`.
Pass `--baseline` at a nonexistent path so the harness skips the WS1
baseline comparison (a WS1 baseline against WS2/WS3 queries would
spuriously flag every metric as a regression):

```
locus_retrieval_eval --workspace D:/Projects/LocusTestWorkspaces/WS2_Wikipedia \
  --queries  tests/retrieval_eval/queries_ws2.json \
  --out      tests/retrieval_eval/results_ws2.md \
  --baseline tests/retrieval_eval/baseline_ws2_none.json

locus_retrieval_eval --workspace D:/Projects/LocusTestWorkspaces/WS3_Documents \
  --queries  tests/retrieval_eval/queries_ws3.json \
  --out      tests/retrieval_eval/results_ws3.md \
  --baseline tests/retrieval_eval/baseline_ws3_none.json
```

**No baselines committed for WS2/WS3.** These workspaces aren't on every
dev's disk; the committed `baseline.json` stays WS1-only. If a future
retrieval refactor needs a regression guard on non-code corpora,
reactivate by running `--write-baseline` against a freshly built WS2/WS3
and committing the resulting per-workspace baseline file.

**Pick the embedder profile that matches what you're measuring.** WS2's
committed `results_ws2.md` ran on bge-small (small profile) because
bge-m3 takes ~10 hours to embed 5000 articles on CPU. The numbers there
are honest for that setup -- semantic recall 13/14 at full coverage --
but they hide a known small-embedder limitation: gold queries that share
vocabulary with the article body score well, gold queries that paraphrase
the topic can drop out entirely. Concrete example from the S5.N
acceptance run: *"Which scientist explained how species change over
time?"* surfaces `A/Earth.html` rank-1 on small profile and Darwin
nowhere in top-5, even though `A/Charles_Darwin.html` is indexed and
ranks #1 the moment the query uses "evolution natural selection". See
the "Small-profile paraphrase ceiling" subsection in
[test-workspaces.md](../../test-workspaces.md) for the four-query table.
Run with bge-m3 if your eval needs paraphrase robustness; budget the
embedding wall time accordingly.

## Why no CI hook

The original S4.K plan called for "fail the build on >5% regression". The
harness needs:
- A fully-indexed workspace (~10s on WS1).
- The bge-m3 GGUF (~600 MB) loaded.
- The embedding queue drained (~tens of seconds on WS1).

That's too heavy for "every build". Instead: this is a manual gate run when
touching retrieval code. The exit-code regression check is preserved so a
future CI hook needs only one line.
