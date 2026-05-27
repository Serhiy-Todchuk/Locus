# 0009. Retire `search_hybrid` from the LLM tool surface

- **Status**: Accepted
- **Date**: 2026-05-27

## Context

[ADR-0008](0008-split-unified-searchtool.md) split the unified `search` dispatcher into six per-mode top-level tools (`search_text` / `search_regex` / `search_symbols` / `search_semantic` / `search_hybrid` / `search_ast`). With that done, `search_hybrid` and `search_semantic` sit next to each other in the manifest with overlapping purpose:

- Both take `(query, max_results)`. Identical arg surface.
- Both gate on `capabilities.semantic_search`. Identical capability.
- Both run through the same `apply_reranker` pipeline. Identical reranker integration.
- Both return `SearchResult` with `(path, line, score, snippet)`. Identical output shape.

The only difference is one call into `IndexQuery`: hybrid blends BM25 on top of cosine via reciprocal rank fusion; semantic is pure cosine. The retrieval-eval baselines committed at [tests/retrieval_eval/results_ws2.md](../../tests/retrieval_eval/results_ws2.md) and [tests/retrieval_eval/results_ws3.md](../../tests/retrieval_eval/results_ws3.md) measure both against gold queries on two corpora:

| corpus | text recall@1 | semantic recall@1 | hybrid recall@1 | semantic MRR | hybrid MRR |
|---|---|---|---|---|---|
| WS2 (Wikipedia ZIM, 5000 articles)  | 82.1% | **96.4%** | 82.1% | **1.000** | 0.929 |
| WS3 (RFC PDFs + Markdown notes, 13 q) | 26.9% | 50.0%   | **53.8%** | **0.885** | 0.872 |

WS2 shows semantic dominating hybrid by 14 points on recall@1; hybrid matches text on recall@1 and is the slowest of the three. WS3 shows hybrid edging semantic by 3.8 points on recall@1 -- but losing on MRR, and within noise on coverage (both 13/13). The shape of the disagreement is the load-bearing data point: **hybrid is not a Pareto improvement.** Where it wins, it wins narrowly. Where it loses, it loses badly. And there is no in-query signal the LLM (or a user) could use to tell, ahead of time, which side of that variance a given corpus will land on.

Two LLM-facing concerns layer on top:

1. **Score legibility.** `search_semantic`'s "similarity: 0.873" is a familiar cosine. `search_hybrid`'s "score: 0.4321" is an opaque blend of BM25 (~0-30 range) and cosine (~0-1 range), normalised by RRF. The model can't tell from the number whether a 0.43 hybrid score means "strong BM25, weak semantic", "weak both", or "strong semantic, weak BM25".
2. **Picking ambiguity.** Two adjacent tools with overlapping descriptions ("vector similarity" vs "BM25 + semantic blend") reproduce the F11/F15/F20 picking-failure shape ADR-0008 just solved -- except the model has no clear discriminator to lean on.

## Decision

Retire `SearchHybridTool` from the LLM-facing tool registry. The five remaining per-mode tools (`search_text`, `search_regex`, `search_symbols`, `search_semantic`, `search_ast`) cover the workload.

`IndexQuery::search_hybrid` is kept in code (still callable from the retrieval-eval harness at [tests/retrieval_eval/eval_runner.cpp](../../tests/retrieval_eval/eval_runner.cpp)) so future BM25+vector tuning can be measured against the per-corpus baseline before a re-introduction case is made.

**No back-compat shim.** Locus has no external users yet; saved sessions that referenced `search_hybrid` fail at dispatch with an unknown-tool error. The S6.17 unified-`search` → per-mode rewrite shim in `SessionManager::load` does not get a corresponding `search_hybrid` → `search_semantic` collapse.

## Consequences

### Easier

- One fewer tool in the lazy manifest. Per-tool one-liner saved (~80 chars + the API tools[] entry).
- Two-tool ambiguity between `search_semantic` and `search_hybrid` collapses to one clear option. F11/F15/F20-class picking failures cannot recur on this pair.
- LLM gets a legible score number (cosine) instead of an opaque RRF blend.
- Code: ~70 lines retired from `search_tools.{h,cpp}`. Class deleted, registration line deleted, capability-gating + closest-name + retrieval-stats parsing entries all simplified.

### Harder

- On a future corpus where BM25+vector blend would have won by a large margin, the LLM no longer has hybrid available without a re-introduction. The retrieval-eval harness is the early-warning system: if `IndexQuery::search_hybrid` ever beats `IndexQuery::search_semantic` by 10+ points on a new corpus, that's the signal to revisit.
- One small surprise for anyone with a saved session that fired `search_hybrid` -- it errors at load-replay rather than degrading silently. Acceptable per "no external users yet."

### Gave up

- The "best of both worlds" framing for retrieval. We accept that BM25 access lives in `search_text` (with the per-tool snippet/exhaustiveness profile that suits it) and conceptual access lives in `search_semantic`. If the model wants both, it can fire both calls -- ~500 cumulative tokens, well under the cost of carrying a third tool whose score the model can't reason about.

## Alternatives considered

- **Keep both, document the trade-off in `description()`.** Rejected. The model picks tools from one-line summaries in lazy-manifest mode; long descriptions are unread until `describe_tool` is called. And there is no per-query signal to encode -- the choice is corpus-shape-dependent, not query-shape-dependent.
- **Retire `search_semantic`, keep `search_hybrid`.** Rejected. Hybrid loses to semantic by 14 points on WS2. Bounded downside for semantic-as-default; unbounded downside for hybrid-as-default.
- **Tune hybrid's blend weights per-workspace and keep both.** Plausible future work, but premature -- adds a tuning surface and config knob to retain a tool whose value isn't established. If hybrid comes back, it comes back with measured per-workspace tuning, not as the fixed-weights version retired here.
- **Keep a back-compat session-load shim that rewrites `search_hybrid` → `search_semantic`.** Drafted, then dropped. Locus has no external users yet; the shim is dead weight from day zero. A user-facing dispatch error is the right signal.
