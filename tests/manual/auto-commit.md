# Manual Test Plan -- Per-Turn Auto-Commit

**Feature:** When the workspace is a git repo and the agent makes a file
mutation during a turn, Locus runs `git add -A && git commit` after the
turn yields. The commit subject is the agent's final message, prefixed
with the configured `git_commit_prefix` and truncated at ~200 chars. A
chip in the chat footer surfaces the new commit's short SHA.

**Audience:** QA. Click-by-click; assumes basic familiarity with the chat
panel, footer area, and editing `.locus/config.json` by hand. Requires
`git` on PATH and a tool-calling LLM running in LM Studio.

Test 1 covers the happy path on a fresh git repo. Test 2 covers the
dedicated-branch variant. Test 3 covers the no-git fallback (auto-commit
on but the workspace isn't a git repo -- Locus must remain silent).

---

## What you're looking at

The chat panel's footer (below the input box) carries a row of small chips
to the right of the context meter. The auto-commit chip reads
`Commit: <sha> (<branch>)` and only appears once a commit has actually
landed during the session. Hover for the full commit subject.

The activity panel (right pane, Log tab) shows entries like
`Auto-commit abcd123 on master` alongside tool calls.

---

## Test 1 -- Happy path on the current branch

**Goal:** confirm a successful agent turn that mutates a file produces
exactly one git commit on the current branch, with the chosen prefix.

### Setup

1. Open a terminal and create a fresh git repo:

   ```
   mkdir locus-autocommit-test
   cd locus-autocommit-test
   git init
   git config user.email "you@example.com"
   git config user.name  "You"
   echo "init" > README.md
   git add README.md
   git commit -m "initial"
   ```

2. Launch Locus once against this folder so it creates `.locus/config.json`.
   Quit Locus.

3. Open `.locus/config.json` in your editor. Find the (newly-added) `git`
   block and set:

   ```json
   "git": {
     "auto_commit":   true,
     "commit_branch": "",
     "commit_prefix": "[locus] "
   }
   ```

   Save.

4. Re-launch Locus on the same folder.

### Steps

1. In the chat panel, send a message that will result in a single file
   mutation, e.g.:

   > Create a file called `notes.md` at the workspace root with one line:
   > "Hello from Locus."

2. When the agent's `write_file` (or `edit_file`) call shows up in the
   approval panel, click **Approve**. Wait for the turn to finish.

   - **Expected:** the chat panel's footer shows a new chip:
     `Commit: <7-char-sha> (master)`. Hover -- the tooltip shows the
     prefix + agent message subject.
   - **Expected:** the activity panel logs an entry like
     `Auto-commit abc1234 on master`.

3. Drop back to a terminal, run `git log -1 --pretty=%s`.

   - **Expected:** the latest commit subject begins with `[locus] ` and
     reflects the agent's final message (truncated at ~200 chars if long).

4. In the same terminal, run `git status`.

   - **Expected:** `working tree clean`. The auto-commit captured the
     `notes.md` file.

5. Send a second message asking the agent to do something **non-mutating**,
   e.g. "What's in notes.md?".

   - **Expected:** no new commit lands (the chip's SHA does not change).
     `git log -1` confirms the previous commit is still HEAD.

### Cleanup

Delete the test folder.

---

## Test 2 -- Dedicated agent branch

**Goal:** verify `commit_branch` parks agent commits on a side branch and
that the branch is created automatically the first time.

### Setup

Same as Test 1 setup steps 1 and 2 (fresh git repo + first Locus launch
to populate `.locus/config.json`). Then:

3. Edit `.locus/config.json` and set:

   ```json
   "git": {
     "auto_commit":   true,
     "commit_branch": "locus-agent",
     "commit_prefix": "[locus] "
   }
   ```

   Note: the branch `locus-agent` does NOT exist yet. Save.

4. Re-launch Locus.

### Steps

1. Send a message that mutates a file:

   > Create a file `notes2.md` containing the text "Side branch test".

   Approve the tool call. Wait for the turn to finish.

   - **Expected:** the footer chip reads
     `Commit: <sha> (locus-agent)` -- not `(master)`.
   - **Expected:** Locus's log (`.locus/locus.log`) contains a line
     `auto_commit: created branch 'locus-agent' (was missing)` (warning
     level) the first time.
   - **Expected:** the activity panel logs `Auto-commit ... on locus-agent`.

2. In a terminal:
   - `git branch` -- both `locus-agent` and `master` listed.
   - `git log locus-agent -1 --pretty=%s` -- shows the prefixed agent
     subject.
   - `git log master -1 --pretty=%s` -- still the original `initial`
     commit. `master` is unchanged.

3. Send a second mutating turn (e.g. "Create notes3.md saying hello").
   Approve.

   - **Expected:** the chip SHA changes; both new commits are on
     `locus-agent`.
   - **Expected:** no "created branch" warning this time -- the branch
     already exists.

### Cleanup

Delete the test folder.

---

## Test 3 -- No-git fallback

**Goal:** confirm `auto_commit=true` against a non-git folder is a silent
no-op, not an error.

### Setup

1. Create a fresh folder that is **NOT** a git repo (no `git init`).
2. Launch Locus, quit, then edit `.locus/config.json`:

   ```json
   "git": {
     "auto_commit":   true,
     "commit_branch": "",
     "commit_prefix": "[locus] "
   }
   ```

   Save.
3. Re-launch Locus.

### Steps

1. Send a message that mutates a file:

   > Create a file called `placeholder.txt` containing the word "ok".

   Approve the tool call.

   - **Expected:** the turn completes normally. **No** auto-commit chip
     appears in the footer. **No** error popups. **No** activity-panel
     "Auto-commit" entry.
   - **Expected:** Locus's log (`.locus/locus.log`) contains a single
     trace-level line `auto_commit: not a git repo, skipping`.

### Cleanup

Delete the test folder.

---

## Test 4 -- Failure path: read-only repo / hooks reject

**Goal:** confirm a failed `git commit` (e.g. a pre-commit hook rejects)
surfaces as a single warning + activity entry but does NOT block the agent
or cause a crash.

### Setup

1. Same as Test 1 setup steps 1-4 (fresh git repo with `auto_commit=true`).
2. Add a pre-commit hook that always fails:
   - Create file `.git/hooks/pre-commit` containing:

     ```sh
     #!/bin/sh
     echo "pre-commit hook says no"
     exit 1
     ```

   - On Windows, you may also need to make sure git can find a shell to
     run the hook (Git for Windows ships one). If your environment
     doesn't have one, skip this test -- the failure path is the same
     for any commit failure (hook, signing rejection, locked refs, ...).

### Steps

1. Send a mutating message and approve the tool call.

   - **Expected:** the turn completes. **No** crash, **no** modal popup.
   - **Expected:** the activity panel shows an `Auto-commit failed` entry
     whose detail mentions "pre-commit hook says no" (or similar).
   - **Expected:** the footer chip does NOT appear (no commit landed).
   - **Expected:** Locus's log contains a single `[warning] auto_commit
     failed: ...` line.

2. The next mutating turn must NOT retry the failed commit -- each turn
   tries fresh. Confirm by sending another mutating message: same
   warning re-appears, not a "retry of previous failure" message.

### Cleanup

Remove the hook (`del .git\hooks\pre-commit` or `rm .git/hooks/pre-commit`),
then delete the test folder.
