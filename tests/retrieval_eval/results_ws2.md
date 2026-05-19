# Locus Retrieval Eval

Generated: 2026-05-18 22:21:43
Workspace: `D:\Projects\LocusTestWorkspaces\WS2_Wikipedia`
Queries:   14

## Aggregate metrics

| Method | R@1 | R@3 | R@5 | R@10 | MRR | nDCG@10 | hit | wall |
|---|---|---|---|---|---|---|---|---|
| search_text | 82.1% | 100.0% | 100.0% | 100.0% | 0.905 | 0.929 | 14/14 | 26 ms |
| search_semantic | 96.4% | 100.0% | 100.0% | 100.0% | 1.000 | 1.000 | 14/14 | 4256 ms |
| search_hybrid | 82.1% | 100.0% | 100.0% | 100.0% | 0.929 | 0.947 | 14/14 | 4311 ms |

_R@K = recall@K (mean across queries). hit = queries with ≥1 expected file in top-K_max. wall = total wall time spent inside this method's calls._

## Per-query -- recall@3 hits

| # | Query | text | semantic | hybrid |
|---|---|---|---|---|
| 1 | what is the capital of Australia | ✓ | ✓ | ✓ |
| 2 | Albert Einstein theory of relativity | ✓ | ✓ | ✓ |
| 3 | ancient Egypt pyramids pharaoh | ✓ | ✓ | ✓ |
| 4 | Byzantine Empire Constantinople fall 1453 | ✓ | ✓ | ✓ |
| 5 | Beethoven classical composer symphonies | ✓ | ✓ | ✓ |
| 6 | Charles Darwin evolution natural selection | ✓ | ✓ | ✓ |
| 7 | DNA double helix structure | ✓ | ✓ | ✓ |
| 8 | Earth orbit around the Sun | ✓ | ✓ | ✓ |
| 9 | French Revolution 1789 storming Bastille | ✓ | ✓ | ✓ |
| 10 | Galileo Galilei astronomy telescope | ✓ | ✓ | ✓ |
| 11 | human heart circulatory system | ✓ | ✓ | ✓ |
| 12 | Industrial Revolution steam engine factories | ✓ | ✓ | ✓ |
| 13 | Japan Tokyo emperor | ✓ | ✓ | ✓ |
| 14 | Krakatoa volcano 1883 eruption | ✓ | ✓ | ✓ |

