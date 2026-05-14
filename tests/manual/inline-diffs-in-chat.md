# Inline Diffs in Chat (S5.C)

Three tests covering the per-tool diff render paths. Diffs render below the matching tool bubble. The Settings > Chat toggle (or hand-editing `chat.show_diffs` in `.locus/config.json`) controls visibility; line cap defaults to 200.

## Test 1: edit_file round-trip + multi-edit batch

1. Open a workspace and ask the agent: "Replace 'todo' with 'TODO' in a small text file" (or any prompt that produces a single-edit `edit_file` call).
2. Approve the call. Under the tool bubble, a `tool-diff` block appears with the file path, a `@@ edit 1/1 @@` header, the `old_string` shown in red with `-` prefix and the `new_string` shown in green with `+` prefix.
3. Ask the agent for a batched edit (e.g. "rename three usages of `foo` to `bar` in this file in one call"). The result block shows three hunks, headers `@@ edit 1/3 @@` through `@@ edit 3/3 @@`, in the same order the agent emitted them.
4. Right-click and copy a diff line -- text comes out clean (no `<span>` markup leaks; the `+` / `-` prefix is from CSS so it isn't copied).

**Expected outcome**: one diff per edit hunk, ordering preserved, copy/paste works. Toggling Settings > Chat > "Show inline diffs" off and asking for another edit suppresses the diff body (the existing `<details>Result</details>` block returns).

## Test 2: write_file overwrite vs new file

1. Ask the agent to create a brand-new file (path that doesn't exist yet).
2. The diff block renders every line as add (green `+`), with no del lines. Header reads `write_file: <path>`.
3. Ask the agent to overwrite an existing file with different content (e.g. "rewrite this small README to be more concise").
4. The diff block shows the actual before/after lines: shared lines as `ctx` (gray, two-space prefix), removed lines red, added lines green. Should be the *minimal* diff -- if you swapped one paragraph, only that paragraph's lines change, not the whole file.
5. Force a no-op overwrite (rewrite with byte-identical content). The block reads `(no changes)`.
6. Force an oversized write (e.g. paste a huge JSON dump as content). Diff truncates at the configured `chat_diff_max_lines` (default 200) with a `(N more lines not shown)` footer.

**Expected outcome**: dtl produces minimal diffs; new-file and no-op edges render cleanly; truncation caps work.

## Test 3: delete_file summary + failure paths

1. Ask the agent to delete a small file. The diff block reads `delete_file: <path> (N lines)` with the file's line count from the checkpoint snapshot. No diff body.
2. Reject the next `edit_file` / `write_file` / `delete_file` call from the approval modal. The result block reads "Error" (red) with the standard `[rejected]` text; no diff renders. Same for `[cancelled]` and `[denied by policy]`.
3. Force a tool failure (e.g. write_file with an out-of-workspace path). The result renders as Error, no diff.

**Expected outcome**: delete_file shows one-line summary with line count from the checkpoint. Failed/rejected/cancelled calls suppress the diff and surface the error text in the result block.
