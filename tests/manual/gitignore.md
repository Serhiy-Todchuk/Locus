# Manual Test Plan -- .gitignore Respect

**Feature:** The indexer reads root and nested `.gitignore` files at
workspace open and on watcher-driven changes, and excludes the matching
paths from the index in addition to the workspace's explicit
`exclude_patterns`. Off-switch: `index.respect_gitignore=false` in
`.locus/config.json` (default `true`).

**Audience:** QA. Click-by-click; assumes basic familiarity with the chat
panel, file tree, activity panel, and editing a `.gitignore` file by hand.
Does NOT require an LLM -- the search tool can be exercised through any
chat input that reaches the indexer.

Test 1 covers the happy path (root + nested patterns, watcher reload).
Test 2 verifies the off-switch behaves as documented.

---

## What you're looking at

The activity panel (right pane, Log tab) shows index events such as
`Excluded N path(s) from index after .gitignore update` whenever the
indexer reloads the gitignore set. The file tree (left pane) reads disk
directly so excluded files still appear there -- it's the search results
and any agent tool that consults the index where excluded paths must
disappear.

---

## Test 1 -- Root + nested + live reload

**Goal:** confirm patterns from a root `.gitignore` AND a nested
`frontend/.gitignore` exclude their respective files; editing
`.gitignore` mid-session re-applies on the next watcher tick.

### Setup

1. Create a fresh folder anywhere convenient, e.g.
   `C:\Users\<you>\Documents\locus-gi-test\`.
2. Inside it, create the following layout (exact paths):
   - `keep.txt` (any content -- e.g. "kept content")
   - `secret.txt` (any content -- e.g. "secret content")
   - `.gitignore` containing exactly: `secret.txt`
   - `frontend\dist\bundle.js` (any content)
   - `frontend\.gitignore` containing exactly: `dist/`

### Steps

1. Launch Locus and open the folder as the workspace.

   - **Expected:** the workspace opens cleanly. The activity panel summary
     shows a small file count (4 files at most -- the two `.gitignore`
     files plus `keep.txt`).
   - **Expected:** Locus's log (`.locus/locus.log`) contains a line
     `gitignore: loaded N pattern(s) from workspace .gitignore files`.

2. In the chat panel, send a message that triggers a search -- for
   example:

   > Search for the word "secret" in this workspace.

   - **Expected:** zero hits returned. `secret.txt` was dropped from the
     index by the root `.gitignore`.

3. Send another search:

   > Search for the word "bundle" in this workspace.

   - **Expected:** zero hits. `frontend/dist/bundle.js` is excluded by the
     nested `.gitignore` in `frontend/`.

4. In your editor, open `.gitignore` (the root one) and add a new line
   `keep.txt`. Save.

   - **Expected (within ~2 seconds):** an activity-panel entry appears:
     `Excluded 1 path(s) from index after .gitignore update`.

5. Send a search for the word "kept" (matching `keep.txt`'s content).

   - **Expected:** zero hits. The file was just dropped from the index.

6. Edit `.gitignore` again, removing the `keep.txt` line you just added.
   Save.

   - **Expected (within ~2 seconds):** an activity-panel entry appears:
     `.gitignore reloaded (N pattern(s))`.
   - **Expected:** a follow-up search for "kept" now returns hits from
     `keep.txt` -- the watcher re-indexed the file once it stopped
     matching the exclude.

### Cleanup

Delete the test folder.

---

## Test 2 -- Off-switch (`respect_gitignore=false`)

**Goal:** verify the off-switch causes Locus to ignore `.gitignore` files
entirely and rely only on `index.exclude_patterns`. Useful for users who
want to index things they intentionally git-ignore (e.g. archived
generated docs).

### Setup

1. Create a fresh folder. Add:
   - `noise.txt` (any content -- e.g. "should be indexed")
   - `.gitignore` containing exactly: `noise.txt`
2. Launch Locus, quit immediately.
3. Open `.locus/config.json` and find the `index` block. Add or set:

   ```json
   "index": {
     "respect_gitignore": false,
     ...other index fields unchanged...
   }
   ```

   Save.
4. Re-launch Locus on the same folder.

### Steps

1. Wait for the initial index to complete (status bar shows the file
   count update).

   - **Expected:** Locus's log shows the pattern-loader did NOT run
     (no `gitignore: loaded N pattern(s)` line).

2. In the chat panel, send a search:

   > Search for the word "should" in this workspace.

   - **Expected:** one hit, in `noise.txt`. The file was indexed despite
     being in `.gitignore` because `respect_gitignore=false`.

3. Edit `.locus/config.json`, set `"respect_gitignore": true`, save,
   restart Locus.

   - **Expected (after restart):** the same search returns zero hits.
     The file is now excluded again. The activity panel may also log
     `Dropped 1 stale row(s) from index after exclude check` from the
     on-open reconciliation pass.

### Cleanup

Delete the test folder.
