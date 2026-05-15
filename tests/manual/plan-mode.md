# Manual Test Plan -- Plan Mode

**Feature:** A three-state agent workflow (`Chat` / `Plan` / `Execute`) where
the LLM proposes a multi-step plan, the user explicitly approves or rejects
it, and only then does the agent run it. Mirrors the workflow in RooCode /
Cline / Claude Code.

**Audience:** QA. Click-by-click; assumes basic familiarity with the
chat panel, mode switcher, and activity panel.

Test 1 runs against a **fresh empty workspace** -- the realistic "user
wants to start a new tiny project" scenario. Test 2 runs against the
**Locus repo itself**, where the agent has real code to read, search, and
report on.

---

## What you're looking at

Above the chat input there are three buttons: **Chat** | **Plan** | **Execute**.
When a plan is proposed, a **plan bubble** appears in the chat with numbered
steps (`pending` pending / `in_progress` in progress / `done` done / `x` failed) and
**Approve** / **Reject** links. Next to the context-token meter at the
footer there's a **plan chip** showing live progress (`Plan: 2/4 -- ...`).

---

## Test 1 -- Reject and refine

**Goal:** verify Plan mode is actually a gate -- the agent only proposes,
never executes -- and a Reject cleanly leaves the workspace untouched.

### Setup

1. Create a brand-new empty folder anywhere convenient, e.g.
   `C:\Users\<you>\Documents\locus-plan-test\`. Confirm it is empty.
2. Launch Locus and open that folder as the workspace.

### Steps

1. Click **Plan**.
2. Paste exactly:

   > Plan a clickable counter web page that I will build in this empty
   > workspace. Create three files at the workspace root: index.html,
   > style.css, and counter.js. The button should say "Click me" and the
   > counter increments on each click. Make this a 4-step plan: one step
   > per file plus a final step that opens index.html to verify. Keep
   > step descriptions one sentence each.

3. Send. Wait for the plan bubble.
4. Confirm:
   - The bubble has **four** numbered steps, each with `pending` (pending) glyph.
   - **Approve** and **Reject** links are visible.
   - The activity panel shows **only** `propose_plan` -- no `write_file`,
     no `list_directory`, no `search`. If anything else fired, that's a
     real bug (mode filter broken); capture the activity-panel screenshot.
5. Click **Reject**.
6. Confirm:
   - The plan bubble locks (Approve / Reject disappear, glyphs stay `pending`).
   - The plan chip never appears in the footer.
   - The mode switcher stays on **Plan**.
   - **The workspace folder is still empty.** Open it in Explorer to
     confirm -- no `index.html`, no anything.
7. Send a follow-up:

   > Try again, but make it a 2-step plan that creates only a single
   > index.html with inline CSS and JS. Keep it minimal.

8. Confirm a fresh plan bubble appears with **two** steps. The previous
   (rejected) bubble is still visible in the chat history but stays locked.
9. Click **Chat** to leave Plan mode without executing. Verify the
   workspace folder is **still empty**.

---

## Test 2 -- "Go wild" multi-tool execute (Locus repo)

**Goal:** stress the execute-mode tool catalog against a real codebase.
One plan should pull in 5 different tools and end with a real file written
to the workspace root.

### Setup

Open the Locus repo (`D:\Projects\AICodeAss\`) as the workspace.

### Steps

1. Click **Chat**, then click **Plan**.
2. Paste this verbatim:

   > Propose a 5-step plan to audit how the agent's tool manifest is built.
   > The plan should:
   >
   > 1. Search the codebase for `ToolRegistry::build_schema_json`.
   > 2. Get the outline of that source file.
   > 3. Read the actual implementation of that function.
   > 4. Run `dir build\release\Release` to confirm the build is up to date.
   > 5. Write a short summary report into `audit-tools.md` at the workspace
   >    root.
   >
   > Set `tools_needed` to the expected tool name on each step
   > (`search`, `get_file_outline`, `read_file`, `run_command`, `write_file`).
   > One sentence per step description.

3. Wait for the plan bubble. Confirm:
   - Five steps, in the requested order.
   - `tools_needed` reads search / get_file_outline / read_file /
     run_command / write_file.
   - Activity panel shows **only** `propose_plan` fired -- nothing else.
4. Click **Approve**. The switcher flips to **Execute**, the plan chip
   activates, and the agent starts running on its own.
5. Watch the activity panel. You should see roughly:

   | Step | Tool | Approval pane pops? |
   |---|---|---|
   | 1 | `search` (text/regex) | no -- auto-approved |
   | 2 | `get_file_outline` | no |
   | 3 | `read_file` | no |
   | 4 | `run_command` | **yes** -- approve it |
   | 5 | `write_file` | **yes** -- approve it |

   Between steps the agent should call `mark_step_done` -- the bubble's
   glyphs tick from `pending` to `done` and the chip advances `1/5 -> 5/5`.
6. When the chip reads `Plan: done (5/5)` (or similar) and the agent's
   final reply lands, open `audit-tools.md` at the workspace root.
   It should exist, be non-empty, and actually reference
   `ToolRegistry::build_schema_json`.
7. Delete `audit-tools.md`, click **Chat** to return to default.

### Soft-pass notes (model-side variance, not bugs)

- **Step glyphs never advance past `pending` / `in_progress`:** small models often skip
  `mark_step_done` even though they're running the steps. As long as the
  tools fire in roughly the right order and the file is written, the test
  passes.
- **Tool order shuffles a bit:** acceptable, the plan is guidance, not a
  strict script. Pass criterion: all 5 distinct tool families fire, file
  exists, content is coherent.

### Hard fails (capture and report)

- The approval pane does **not** pop for `run_command` or `write_file`.
  Settings -> Tool Approvals should have both set to *Ask Every Time*.
- Execute phase never starts after Approve. Capture `.locus/locus.log`
  from the moment you clicked.
- `audit-tools.md` not created or empty after the agent reports done.
