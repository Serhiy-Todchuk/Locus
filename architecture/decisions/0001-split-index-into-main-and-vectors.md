# 0001. Split the workspace index into `index.db` + `vectors.db`

- **Status**: Accepted
- **Date**: 2026-04-16
- **Commit**: e826b0f (`Split semantic storage into .locus/vectors.db`)

## Context

The original design put everything in a single `.locus/index.db`: `files`, `files_fts`, `symbols`,
`headings`, `chunks`, and the `vec0` `chunk_vectors` virtual table, all sharing one SQLite
connection and one WAL.

Two problems surfaced once semantic indexing ran in the background:

1. **Write serialisation.** SQLite serialises writers per database file. The embedding worker
   doing `INSERT INTO chunk_vectors` held the WAL long enough to stall the indexer's FTS/symbol
   updates on file-change events — exactly the foreground path that must feel instant.
2. **Coupled lifetimes.** The vector data is a *derived cache*: it depends on chunk text and the
   embedding model. Changing models or reclaiming disk should not require touching the skeleton
   index. With a single file there was no clean way to blow away only the vectors.

There was also a latent third problem: users with semantic search disabled were still paying the
cost of loading the `sqlite-vec` extension and carrying vector columns in their index.

## Decision

Split storage into two SQLite files in `.locus/`, each with its own `Database` connection:

- `index.db` — always present. Holds `files`, `files_fts`, `symbols`, `headings`. Schema kind
  `Main`.
- `vectors.db` — created lazily, only when semantic search is enabled and an embedding model is
  available. Holds `chunks` and the `vec0` `chunk_vectors` virtual table. Schema kind `Vectors`.
  The `sqlite-vec` extension is loaded **only** on this connection.

Semantic search becomes a two-step read: query `vectors.db` for top-K `chunk_id` → `file_id`,
then resolve `file_id` → `path` in `index.db`. Both lookups are cheap (small K, single-row PK
fetches). Cross-DB `file_id` is a logical reference, not a SQL FK — orphaned chunks are cleaned
up on the next re-index of that file.

## Consequences

**Wins**
- Foreground FTS/symbol writes no longer wait on the embedding worker's transactions. Two WALs,
  two write queues.
- `rm .locus/vectors.db` reclaims disk and forces a clean re-embed without disturbing the
  skeleton index. Changing embedding models is now a one-liner.
- Users with semantic search off pay zero cost: `vectors.db` is never created, `sqlite-vec` is
  never loaded.

**Costs**
- Two connections to manage in `Workspace`. `Database::DbKind` distinguishes them.
- Cross-DB lookups cannot be a single JOIN. In practice every semantic query already resolved
  `chunk_id` → `file_id` → `path` separately, so this is negligible.
- A legacy-table migration had to be added for workspaces indexed before the split.

## Alternatives considered

- **Keep one file, use `BEGIN IMMEDIATE` + smaller transactions in the embedding worker.**
  Reduces but does not eliminate stalls — any ongoing vector write still blocks FTS writes,
  and the coupled-lifetime problem remains.
- **`ATTACH DATABASE` to present both files as one logical DB.** Useful for queries, but
  attached DBs still share the main connection's transaction — does not solve the writer
  serialisation.
- **Move vectors out of SQLite entirely (flat file / hnswlib / faiss).** Overkill for the current
  scale (tens of thousands of chunks per workspace), loses SQL composability, and `sqlite-vec`
  is already good enough for exact + approximate KNN at this size.
