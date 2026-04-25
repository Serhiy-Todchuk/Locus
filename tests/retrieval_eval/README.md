# Retrieval Evaluation Harness (S4.K)

Manual-only harness that measures retrieval quality on a real workspace. Lets
us tell whether a chunker tweak, embedder swap, or RRF parameter change
actually helps — instead of shipping retrieval on vibes.

## What it measures

For each gold query in [queries.json](queries.json), runs `search_text`,
`search_semantic`, and `search_hybrid` and computes:

- **recall@K** for K ∈ {1, 3, 5, 10} — fraction of expected files retrieved in
  the top K
- **MRR** — mean reciprocal rank of the first hit
- **nDCG@10** — discounted cumulative gain (binary relevance)
- **wall time** — total time spent in each method's calls

Granularity is file-level (one path per relevant document). Symbol-level
gold is intentionally not modelled here — `search_symbols` is a different beast
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
2. Wait for the embedding queue to drain (capped at 600s — set `--no-wait` to
   skip).
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
Keep `expected_files` tight — 1–3 paths per query. A query that lists 10
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

## Why no CI hook

The original S4.K plan called for "fail the build on >5% regression". The
harness needs:
- A fully-indexed workspace (~10s on WS1).
- The bge-m3 GGUF (~600 MB) loaded.
- The embedding queue drained (~tens of seconds on WS1).

That's too heavy for "every build". Instead: this is a manual gate run when
touching retrieval code. The exit-code regression check is preserved so a
future CI hook needs only one line.
