## Manual Test Plan -- `@`-Mention File References

**Feature:** Type `@` in the chat input to get an autocomplete dropdown of
indexed workspace files. Selecting a file inserts the workspace-relative
path. On send, the first `@<path>` token that matches a known indexed file
is automatically pinned as the attached context (same single-file slot the
right-click "Attach to context" already uses); the path text stays in the
message verbatim.

**Trigger heuristic:** `@` only fires the popup when it's at the start of
the input or preceded by whitespace / common punctuation -- `user@example.com`
won't pop the dropdown.

**Single-slot model:** Locus currently supports one attached file at a time.
If a message has multiple `@` mentions, only the first resolved one is
attached (last-write wins if the user follows up with another `@<path>`
message). The detach "✕" on the chip works unchanged.

---

### Test 1 -- Type, pick, send, attach

1. Open the Locus repo as a workspace.
2. Click in the chat input and type `@cha`. A dropdown appears above the
   input with paths whose basenames start with "cha" near the top
   (e.g. `chat_panel.h`, `chat_panel.cpp`).
3. Press `Down` to highlight `chat_panel.cpp`, then press `Tab` (or
   `Enter`, or click the row). The input text becomes
   `@src/frontends/gui/chat_panel.cpp ` and the popup hides.
4. Type ` summarise this file.` then press `Enter`.

**Expected**

- The user bubble shows the full message including the `@src/.../chat_panel.cpp`
  token verbatim.
- The "📎" attached-file chip above the input lights up with
  `src/frontends/gui/chat_panel.cpp`.
- The LLM response references the actual contents of `chat_panel.cpp`
  (the attached-context block prepended to the user turn injects the
  file's outline + preview, same as right-click → Attach).
- Clicking the chip's "✕" detaches the file.

---

### Test 2 -- Email-style `@` does NOT trigger

1. Clear the input. Type `email me at user@example.com about this`.
2. Watch the input as you type past `user@`.

**Expected**

- No popup appears when you type the `@` in `user@`. The character
  before `@` is `r` (alphanumeric), so the heuristic suppresses the
  popup.
- Press Enter. No attach chip appears; the message goes through as
  plain text with no auto-attach.

---

### Test 3 -- Trailing punctuation and bad paths

1. Type `please diff @CLAUDE.md against @nonexistent_file.xyz.` and press Enter.

**Expected**

- The user bubble shows the message verbatim, including the trailing
  period after `.xyz`.
- The attach chip lights up with `CLAUDE.md` (the first mention that
  resolves to an indexed file). `nonexistent_file.xyz` is ignored
  because it isn't in the workspace index.
- Hard fail: if the chip shows `CLAUDE.md.` (with the period), the
  trailing-dot stripper in the parser is wrong.
