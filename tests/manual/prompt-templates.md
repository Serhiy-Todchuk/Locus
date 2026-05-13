# Manual Test Plan -- Prompt Templates

**Feature:** Markdown files that act like text macros. Typing `/<name>` in the
chat expands the matching `.md` file's body and sends it to the agent as if
the user had typed that body directly. Templates can hold placeholders
(`{0}`, `{1}`, `{key}`) that get filled from the slash command's arguments.
No tool calls run during expansion -- it's pure text substitution; the
agent then decides whether to call tools as it would for any user message.

**Audience:** QA. Click-by-click; assumes basic familiarity with the chat
panel, the activity panel, and editing `.md` files in a text editor.

**Estimated time:** ~10 minutes for the full plan; ~3 minutes for Tests 1+2.

---

## What you're looking at

- **`<workspace>/.locus/prompts/<name>.md`** -- project-local templates.
  Available only when this workspace is open.
- **`%APPDATA%/Locus/prompts/<name>.md`** -- global templates. Available
  in every workspace this user opens.
- **`/<name>` slash command** -- chat-input slash that expands the template
  whose filename (minus `.md`) matches `<name>`.
- **`/reload` slash command** -- re-scans both prompt directories. New
  files added while Locus is running need this; edits to known files are
  picked up automatically on the next use (mtime check).
- **`/help`** -- now also shows two extra sections, *Templates (project)*
  and *Templates (global)*, listing every visible template with its
  declared args.

**Frontmatter (optional, at the very top of the `.md`):**

```
---
description: One-line hint shown in /help
args: [name_of_arg_0, name_of_arg_1]
---
<body>
```

`description` is displayed under the template name in `/help`. `args`
declares positional argument *names* (cosmetic — the body still references
them as `{0}`, `{1}`, ...).

**Substitution rules:**

- `{0}`, `{1}`, ... -- positional. `/foo bar baz` -> `{0}` = `bar`, `{1}` = `baz`.
- `{key}` -- named. `/foo key=value` -> `{key}` = `value`.
- Missing references are rendered **literally** -- e.g. `{name}` with no
  `name=` argument shows up as the four characters `{name}` in the body, so
  you can see what was unresolved.
- `{{` and `}}` are escapes for literal `{` and `}`.

**Precedence:** built-in slashes (`/help`, `/undo`, `/memorize`, `/forget`,
`/metrics`, `/export_metrics`, `/reload`) win over tools, and tools win over
templates. A template named `compact` cannot shadow `/compact`.

---

## Test 1 -- Project template round-trip

**Goal:** drop a template, invoke it, watch the agent receive the
substituted body and respond.

### Setup

1. Open any workspace (the Locus repo `D:\Projects\AICodeAss\` is fine).
2. In Explorer, navigate to the workspace's `.locus/prompts/` folder.
   If it doesn't exist, create it.

### Steps

1. Create a file `.locus/prompts/echo_marker.md` with this exact body:

   ```
   Repeat exactly this token in your reply with no other words:
   QA-MARKER-{0}-END
   ```

2. In the chat input, type:

   > `/reload`

   Send. The chat should show a confirmation line like
   *"Reloaded prompt templates: 1 available."*. (If there were already
   global templates installed, the count is higher.)

3. In the chat input, type:

   > `/echo_marker 7`

   Send.

4. Watch the chat. The agent's reply should contain the string
   `QA-MARKER-7-END` (the `{0}` placeholder was replaced with `7`).

5. Open the activity panel (right pane). Find the most recent
   `user_message` event. Its summary should read
   *"Prompt template /echo_marker expanded (… chars)"* and the detail
   shows the substituted body (with `{0}` already replaced).

### Expected

- Chat reply contains `QA-MARKER-7-END`.
- Activity panel labels the message as a template expansion.
- The chat input was treated as a single user turn (one assistant reply,
  not two).

### Hard fails

- The agent literally reads back `{0}` instead of `7`.
- The agent replies "Unknown command '/echo_marker'" -- you forgot to
  `/reload` after creating the file (try the slash again after `/reload`).
- The chat shows multiple replies for one slash invocation.

### Cleanup

Leave `echo_marker.md` in place; Test 2 uses it.

---

## Test 2 -- Frontmatter shows up in `/help`

**Goal:** verify the description + args block is parsed and rendered.

### Setup

The `echo_marker.md` from Test 1 is still in place.

### Steps

1. Open `.locus/prompts/echo_marker.md` in a text editor and add a
   frontmatter block at the top so the file reads:

   ```
   ---
   description: Repeat a QA marker back at me
   args: [marker_id]
   ---
   Repeat exactly this token in your reply with no other words:
   QA-MARKER-{0}-END
   ```

   Save.

2. The registry picks up file edits automatically via mtime — no
   `/reload` needed. In the chat, type:

   > `/help`

   and send.

3. Scroll the chat output. Below the built-in tool list, find a
   **"Templates (project):"** section. Confirm:
   - There's a line `/echo_marker <marker_id>` (the `<marker_id>` reflects
     the declared arg name).
   - Underneath, the description text: *"Repeat a QA marker back at me"*.

### Expected

- Description appears verbatim.
- Argument label `<marker_id>` is shown next to the slash name.

### Hard fails

- The template appears in `/help` but with no description ⇒ frontmatter
  parser failed silently. Check that the file starts on line 1 with three
  dashes (`---`) followed by a newline.
- The whole frontmatter block leaks into the chat reply when you invoke
  the template ⇒ frontmatter is being treated as body. Verify there's a
  closing `---` line.

---

## Test 3 -- Missing references render literally

**Goal:** confirm the "no surprise expansion" rule.

### Setup

Standard workspace. The `echo_marker.md` from Test 1 is fine.

### Steps

1. Create `.locus/prompts/missing_test.md` with body:

   ```
   First {0}; second {1}; named {name}; placeholder {nope}.
   ```

2. In the chat:

   > `/reload`

3. In the chat:

   > `/missing_test only_one_arg name=Locus`

   Send.

4. Find the corresponding `user_message` in the activity panel and
   expand its detail. The body sent to the LLM should read:

   > `First only_one_arg; second {1}; named Locus; placeholder {nope}.`

   Note: `{1}` and `{nope}` come through verbatim because no
   corresponding argument was supplied.

### Expected

- `{0}` -> `only_one_arg`
- `{1}` -> literal `{1}` (no second positional)
- `{name}` -> `Locus` (from `name=Locus`)
- `{nope}` -> literal `{nope}` (no `nope=`)

### Hard fails

- Missing references collapse to empty string (`First only_one_arg;
  second ; named Locus; placeholder .`). The "see what wasn't resolved"
  contract is broken.
- The slash errors out instead of expanding partial.

### Cleanup

Leave `missing_test.md` and `echo_marker.md`; Test 4 uses them.

---

## Test 4 -- Built-ins and tools take precedence over a template

**Goal:** verify a malicious template name can't shadow a built-in slash
or a real tool.

### Setup

The two templates from previous tests are present.

### Steps

1. Create `.locus/prompts/help.md` with body:

   ```
   This template should NEVER expand because /help is a built-in.
   ```

2. Create `.locus/prompts/read_file.md` with body:

   ```
   This template should NEVER expand because read_file is a tool.
   ```

3. In the chat:

   > `/reload`

4. Type `/help` and send. Confirm the standard help text appears
   (built-in slash kept precedence), NOT the body of the template.

5. Type `/read_file CLAUDE.md` and send. Confirm the `read_file` tool
   runs (its output is the first part of CLAUDE.md), NOT the template
   body.

6. As a sanity check, type `/echo_marker 99` and send -- it should
   still expand and the agent should answer with `QA-MARKER-99-END`.

### Expected

Built-ins and tools always win on collision. The templates with the
colliding names are reachable in `/help` (under *Templates (project):*
they appear -- look closely) but cannot be invoked from the chat.

### Hard fails

- `/help` outputs the template body. Critical -- escalate.
- `/read_file CLAUDE.md` outputs the template body instead of file
  contents.

### Cleanup

Delete `help.md` and `read_file.md` from `.locus/prompts/` once the
test passes. Leave `echo_marker.md` if you want to keep using it; the
QA-marker pattern is fine to keep around.

---

## Test 5 -- Global templates (cross-workspace)

**Goal:** verify the per-user global directory is consulted, and that
project entries override globals on name collision.

### Setup

1. Locate your global prompts directory. On Windows, that's
   `%APPDATA%\Locus\prompts\`. Open `cmd.exe` and run `echo %APPDATA%`
   if you're not sure where that resolves -- typically
   `C:\Users\<you>\AppData\Roaming\`. Create the `Locus\prompts\` path
   if it doesn't exist.

2. Drop a file `global_marker.md` there with body:

   ```
   GLOBAL_TEMPLATE_OK
   ```

3. In the same global folder, drop `echo_marker.md` (note: same name
   as the project template from Test 1) with body:

   ```
   GLOBAL_ECHO_FALLBACK
   ```

### Steps

1. In Locus, type `/reload` and send. The count should rise by 1 or 2
   depending on what else is present.

2. Type `/help` and send. Confirm:
   - *Templates (project):* group lists `echo_marker` (the project one).
   - *Templates (global):* group lists `global_marker`.
   - `echo_marker` does **not** appear in the global group (project
     entries shadow same-named global entries).

3. Type `/global_marker` and send. Agent receives the body
   `GLOBAL_TEMPLATE_OK` as its user message; assert it appears in
   the activity panel's `user_message` detail.

4. Type `/echo_marker 5` and send. Agent should still get
   `QA-MARKER-5-END` (the project body), not the global fallback.

### Expected

- Global template is invokable across workspaces.
- Project template wins on collision.
- `/help` groups are correct.

### Hard fails

- Global template never shows up in `/help` despite being on disk.
  Check that the dir is exactly `%APPDATA%\Locus\prompts\` (capital L,
  trailing `\prompts\`).
- The global body leaks through when a same-named project template
  exists.

### Cleanup

Delete `%APPDATA%\Locus\prompts\global_marker.md` and
`%APPDATA%\Locus\prompts\echo_marker.md` if you don't want them
sticking around.

---

## Test 6 -- Half-context warning on oversized template

**Goal:** confirm the token-budget guard fires when a template alone
would already eat more than half the model's context window.

### Setup

1. Open your test workspace.
2. Note your model's context window: open Settings -> LLM and read the
   *Context limit* field. Call this `N`. For Gemma 4 E4B that's 8192.

### Steps

1. Create `.locus/prompts/huge.md` with a body roughly half the
   context tokens long. Cheapest way: open any large source file
   (e.g. `src/agent/agent_core.cpp`), copy the first ~`N/2` characters,
   paste them into `huge.md`. (The token estimator uses ~4 chars per
   token; for `N=8192` that means roughly **16 000 characters** of
   plain text.)
2. In the chat, type `/reload` and send.
3. Type `/huge` and send.
4. Watch the chat for an inline **warning line** like:
   *"warning: prompt template '/huge' expanded to ~XXXX tokens (>4096,
   half of context 8192)."*
5. The agent should still proceed -- the warning is informational, not
   a hard block. Cancel the turn (red square or Ctrl+C) once you've
   confirmed the warning fired; you don't actually need to wait for
   the response.

### Expected

- Warning line fires with the rough token count and the half-context
  threshold quoted.
- Turn still runs (the hard context limit is the existing
  `ContextBudget` cutoff, not this warning).

### Hard fails

- No warning despite the body clearly exceeding half the context.
  Possible token estimator regression -- file a bug.
- The turn refuses to run.

### Cleanup

Delete `.locus/prompts/huge.md`.

---

## Test 7 -- Unknown slash with no matching template

**Goal:** the safety net -- a typo doesn't silently dispatch to the LLM.

### Setup

Standard workspace, no template named `definitely_not_a_command`.

### Steps

1. Type `/definitely_not_a_command` and send.
2. Confirm a chat **error line** appears: *"Unknown command
   '/definitely_not_a_command'. Type /help for available commands."*
3. Confirm the activity panel shows **no** `user_message`, **no**
   `llm_request`, **no** `tool_call` for this input. The LLM was not
   invoked.

### Expected

Error line on the error channel, no LLM round.

### Hard fails

- The typo is silently sent to the LLM as a regular user message (you'll
  see a `user_message` event and an assistant reply).
- No error feedback at all -- silent swallow.

---

## Cross-test gotchas

- **/reload is the only way to pick up new files.** Existing files'
  edits are seen automatically via mtime; brand-new files added while
  Locus is running need `/reload`. Workspace switch and Locus restart
  both trigger a full re-scan.
- **Templates are pure text expansion.** No execution, no tool calls,
  no LLM passes happen *during* expansion. The agent then decides what
  to do with the resulting user message as it would for any input.
- **No conditionals, no loops.** If you need logic, you need a real
  extension (deferred to S6.7 Skills). The spec calls this out by
  design.
- **Frontmatter must start on line 1.** Leading blank lines disable
  detection and the whole frontmatter block leaks into the body.
- **`.locus/prompts/` is excluded from the index by default**, so
  template files don't show up in search results or the file tree.
- **Embeddings are not used.** Template lookup is exact name match
  against the filename; there's no fuzzy matching by description.
