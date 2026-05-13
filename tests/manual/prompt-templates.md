# Manual Test Plan -- Prompt Templates

**Feature:** Markdown files that act as text macros. `/<name>` in the chat
expands `.md` body and sends it to the agent as the user's message. Pure
text expansion — no tools, no LLM passes — the agent then decides what to
do with the resulting input.

**On-disk:**

- `<workspace>/.locus/prompts/<name>.md` — project, wins on collision.
- `%APPDATA%\Locus\prompts\<name>.md` — global, available in every workspace.

**Body rules:**

- `{0}`, `{1}`, ... = positional args from the slash; `{key}` = `key=value` args.
- Unresolved references render **literally** (`{name}` stays as `{name}`).
- `{{` / `}}` escape to literal `{` / `}`.
- Optional frontmatter at the very top:

  ```
  ---
  description: One-line hint shown in /help
  args: [arg0_name, arg1_name]
  ---
  ```

**Precedence:** built-in slashes (`/help`, `/undo`, `/memorize`, `/forget`,
`/metrics`, `/export_metrics`, `/reload`) > tools > templates. A template
named `compact` can't shadow `/compact`.

**Cache:** edits to known files are picked up automatically (mtime check).
New files need `/reload`. Workspace switch / Locus restart re-scans fully.

---

## Test 1 -- Project template end-to-end

Covers: round-trip, frontmatter, `/reload`, `/help` rendering, positional +
named + missing substitution, precedence over built-ins and tools, unknown-slash
error.

### Setup

Open any workspace (Locus repo is fine).

### Steps

1. Create three files under `<workspace>\.locus\prompts\`:

   - `echo.md`:

     ```
     ---
     description: Echo a QA marker
     args: [marker_id]
     ---
     Reply with exactly: QA-{0}-END; named:{name}; missing:{nope}
     ```

   - `help.md` (collides with built-in):

     ```
     This should NEVER expand.
     ```

   - `read_file.md` (collides with a tool):

     ```
     This should NEVER expand either.
     ```

2. In chat: `/reload`. Confirmation line shows the new count (≥ 3).

3. `/help`. Output shows the standard help text **plus** a *Templates
   (project):* section listing `/echo <marker_id>` with the description
   *"Echo a QA marker"*. `help` and `read_file` may appear in that
   section too, but their bodies do **not** replace `/help` or
   `/read_file` -- see step 5.

4. `/echo 7 name=Locus`. The agent's reply contains
   `QA-7-END; named:Locus; missing:{nope}`. In the activity panel the
   most recent `user_message` has the summary
   *"Prompt template /echo expanded (… chars)"*, and its detail is the
   substituted body the LLM received.

5. `/help` again -- still the real help text, not the `help.md` body.
   `/read_file CLAUDE.md` -- runs the tool and shows file content, not
   the `read_file.md` body.

6. `/definitely_not_a_command`. Chat shows an error line:
   *"Unknown command '/definitely_not_a_command'. …"*. No
   `user_message`, no `llm_request`, no `tool_call` in the activity
   panel for this input.

### Hard fails

- Agent literally echoes `{0}` instead of `7` (substitution broken).
- Missing references collapse to empty string instead of staying literal.
- `/help` outputs the template body (built-in precedence broken — escalate).
- `/read_file CLAUDE.md` outputs the template body (tool precedence broken).
- Unknown slash is silently dispatched to the LLM as a regular message.

### Cleanup

Delete `echo.md`, `help.md`, `read_file.md` from `.locus/prompts/`.

---

## Test 2 -- Global templates + mtime hot-reload

Covers: `%APPDATA%\Locus\prompts\` is consulted, project wins on collision,
file edits picked up without `/reload`, half-context warning fires on
oversized bodies.

### Setup

1. Open `cmd.exe` and run `echo %APPDATA%` to confirm the path. Create
   `%APPDATA%\Locus\prompts\` if it doesn't exist.
2. Note your model's context limit (Settings → LLM → *Context limit*).
   Call this `N` (Gemma 4 E4B = 8192).

### Steps

1. Drop `%APPDATA%\Locus\prompts\global_only.md`:

   ```
   GLOBAL_ONLY_OK
   ```

   And `%APPDATA%\Locus\prompts\shared.md`:

   ```
   GLOBAL_VERSION
   ```

   Drop `<workspace>\.locus\prompts\shared.md`:

   ```
   PROJECT_VERSION
   ```

2. `/reload`. `/help` should list `global_only` under *Templates
   (global):* and `shared` under *Templates (project):* (the global
   `shared` is shadowed — it should NOT appear under global).

3. `/global_only` -- agent receives `GLOBAL_ONLY_OK`. `/shared` --
   agent receives `PROJECT_VERSION`, never the global fallback.

4. Edit `<workspace>\.locus\prompts\shared.md` in a text editor and
   change the body to `PROJECT_VERSION_V2`. Save. Do **not** call
   `/reload`. Invoke `/shared` again -- the activity-panel detail of
   the resulting `user_message` shows `PROJECT_VERSION_V2` (mtime
   auto-pickup).

5. Half-context warning: open a large source file (e.g.
   `src\agent\agent_core.cpp`), copy the first ~`N/2 * 4` characters
   (~16000 chars for `N=8192`) and paste them into
   `<workspace>\.locus\prompts\huge.md`. `/reload`, then `/huge`.
   The chat shows an inline warning like *"warning: prompt template
   '/huge' expanded to ~XXXX tokens (>4096, half of context 8192)."*
   The turn still runs — cancel it once the warning has fired; you
   don't need to wait for the LLM response.

### Hard fails

- Global template doesn't show up in `/help` despite being on disk —
  check the path is exactly `%APPDATA%\Locus\prompts\`.
- `/shared` returns `GLOBAL_VERSION` (project-wins broken).
- Step 4 returns `PROJECT_VERSION` instead of `V2` (mtime cache stale).
- Step 5 produces no warning despite a clearly oversized body.

### Cleanup

Delete `global_only.md`, `shared.md`, `huge.md` from both prompt dirs.
