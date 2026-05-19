# Locus Retrieval Eval

Generated: 2026-05-19 13:36:59
Workspace: `D:\Projects\LocusTestWorkspaces\WS3_Documents`
Queries:   13

## Aggregate metrics

| Method | R@1 | R@3 | R@5 | R@10 | MRR | nDCG@10 | hit | wall |
|---|---|---|---|---|---|---|---|---|
| search_text | 26.9% | 34.6% | 34.6% | 34.6% | 0.423 | 0.349 | 6/13 | 4 ms |
| search_semantic | 50.0% | 88.5% | 100.0% | 100.0% | 0.885 | 0.879 | 13/13 | 522 ms |
| search_hybrid | 53.8% | 92.3% | 100.0% | 100.0% | 0.872 | 0.883 | 13/13 | 529 ms |

_R@K = recall@K (mean across queries). hit = queries with ≥1 expected file in top-K_max. wall = total wall time spent inside this method's calls._

## Per-query -- recall@3 hits

| # | Query | text | semantic | hybrid |
|---|---|---|---|---|
| 1 | which RFC defines HTTP/3 QUIC | · | ✓ | ✓ |
| 2 | TCP three-way handshake sliding window | ✓ | ✓ | ✓ |
| 3 | Ethernet address resolution protocol | ✓ | ✓ | ✓ |
| 4 | DNS resource records A AAAA CNAME MX | · | ✓ | ✓ |
| 5 | JSON data interchange format specification | ✓ | ✓ | ✓ |
| 6 | TLS 1.3 zero round trip handshake forward secrecy | · | ✓ | ✓ |
| 7 | HTTP/2 binary framing multiplexing HPACK | ✓ | ✓ | ✓ |
| 8 | UDP user datagram protocol connectionless | · | ✓ | ✓ |
| 9 | IPv4 internet protocol best effort datagram | ✓ | ✓ | ✓ |
| 10 | HTTP/1.1 message syntax current specification | · | ✓ | ✓ |
| 11 | evolution of HTTP versions notes | ✓ | ✓ | ✓ |
| 12 | OSI layer 4 transport protocols notes | · | ✓ | ✓ |
| 13 | open questions reading list TODO | · | ✓ | ✓ |

