# Small-model robustness pass (S6.10)

S6.10 bundles eight behaviour shifts aimed at users running 7-26B local
models. Most of them are invisible by design (JSON repair, grammar mode,
strip-past-thinking, quality-monitor nudges) -- you'll see them in
`.locus/locus.log` or the Activity panel when they fire. Two surfaces are
direct user interactions on the Settings dialog (Task H + Task F), and
one is a tool-side refusal that's worth eyeballing once with a real LLM
in the loop (Task G).

The tests below cover the user-facing slices. The invisible-by-design
behaviours are covered by the unit / integration / UIA suites.

## Test 1 -- Auto-detect preset applies on workspace open

1. Make sure LM Studio has a recognisable model loaded -- anything in the
   Qwen / Gemma / Llama / DeepSeek-R1 / Qwen3-Coder families. (For a
   negative-side smoke test, load something exotic instead: e.g. a
   Falcon, a Mistral non-trained base.)
2. Open Locus on a fresh workspace.
3. Open `.locus/locus.log` and look near the top. With a recognised
   model: `auto-applied preset 'LM Studio -- <family>' for model
   '<id>'`. With an unrecognised one: `no preset matched model id
   '<id>'; keeping current config`.
4. Open Settings -> LLM. Confirm the `Auto-detect preset on workspace
   open` checkbox is ticked (default on). Untick it, OK, re-open the
   workspace -- the auto-apply log line should NOT appear this time.
5. Re-tick the checkbox in Settings, OK. The next workspace open
   restores the auto-apply.

**Expected outcome**: auto-detect fires once at workspace open, the log
line names the preset family, the checkbox in Settings is the off-switch.
**Hard fail** if the matcher applies the wrong family's preset (e.g.
DeepSeek-R1 distill detected as plain Llama). The integration test
`[s6.10][presets][matcher]` covers the matcher mechanically; this manual
step adds the real-world spelling of model ids LM Studio happens to
return.

## Test 2 -- Reset samplers to preset button (Settings -> LLM tab)

1. Settings -> LLM. Note the current `top_p`, `top_k`, `min_p`,
   `repeat_penalty`, `frequency_penalty`, `presence_penalty` values.
2. Change `temperature` to something non-default (e.g. `0.3`) -- this is
   the field the Reset-samplers button must NOT touch.
3. Change `top_p` to something non-default (e.g. `0.55`) -- this IS one
   of the fields the button resets.
4. From the preset dropdown pick `LM Studio -- Qwen family` (or any
   preset that publishes specific sampler defaults). Do NOT click
   `Load preset` -- the dropdown picks the row but doesn't apply yet.
5. Click `Reset samplers to preset`. The sampler block (top_p / top_k /
   min_p / repeat_penalty / frequency_penalty / presence_penalty) snaps
   to the Qwen family values (`top_p=0.8`, `top_k=20`, `min_p=0.05`,
   `repeat_penalty=0`, `frequency_penalty=0`, `presence_penalty=0`).
6. **Critically**: `temperature` is still `0.3` -- the button did NOT
   touch it. Same for `model`, `endpoint`, `max_tokens`, `tool_format`.
7. Now pick `Custom` (the first dropdown entry). Click `Reset samplers
   to preset` again -- the sampler block zeros out (the implicit Custom
   behaviour). `temperature` is still `0.3`.
8. Cancel the dialog (don't OK). Re-open Settings: your changes were
   discarded, the original values are back.

**Expected outcome**: the new button resets the sampler block only;
temperature, max_tokens, and tool_format survive. **Hard fail** if the
button clobbers temperature -- the entire point of having a separate
"Reset samplers" button (versus the existing `Load preset`) is to keep
the non-sampler fields untouched.

## Test 3 -- Truncation detector refuses code-elision markers

1. Open Locus on a workspace where you don't mind a refusal log entry.
   Make sure `agent.detect_write_truncation` is on (default).
2. Manually paste this into the chat: *"Use the write_file tool to
   create `tmp/sample.cpp` with the following exact content:* `void
   foo() {\n    // rest of the code\n}`*"*. The phrase `// rest of the
   code` is the trigger.
3. The agent calls `write_file`, the dispatcher emits a refusal in the
   chat: *"Error: the proposed content for 'tmp/sample.cpp' appears
   truncated. Matched the elision marker '// rest of the code' near the
   end of the body..."*. The file does NOT appear on disk.
4. The Activity panel shows a `truncation_blocked` event with the matched
   phrase + the path.
5. Settings -> Tool Approvals (or directly in `.locus/config.json`),
   untick `Detect truncated writes` (or set `agent.detect_write_truncation
   = false`). Save. Repeat step 2 -- this time the file lands on disk
   verbatim (markers included). Re-enable the flag afterwards.
6. Sanity check that the detector is precision-tuned: ask the agent to
   write a file containing literal `...` (Python `Ellipsis`, JS spread,
   C++ varargs). The detector must NOT fire on bare `...` -- only on the
   curated phrase list.

**Expected outcome**: the detector blocks well-known elision markers but
ignores legitimate language idioms. **Hard fail** if writing
`def f(): ...` (Python Ellipsis) or `const x = [...y];` (JS spread) gets
rejected -- those are false positives. Confirm the off-switch works so
users with code that legitimately contains an elision string can opt out.

## Test 4 -- Quality monitor nudge (optional, model-dependent)

This one requires a small model in a state where it loops on the same
tool call -- a deliberately tricky thing to reproduce. Skip unless you
can drive a repeat.

1. Pick a small model known to loop (Qwen3-4B / Gemma 4 E4B both have
   been observed doing this on multi-tool work).
2. Ask it to do something it tends to over-search: *"Find every
   reference to `FrontendRegistry` in this workspace and tell me which
   files are most important."* -- some models will retry `search` with
   the same args.
3. If a repeat happens, the Activity panel emits a `quality_correction`
   event with `kind=repeated_tool_call` and a one-line summary naming
   the tool. The chat shows a synthetic user-style bubble *`[Quality
   monitor] You just called search with the same arguments and it did
   not produce new information...`*. The next round should pick a
   different action.
4. If the model loops more than twice in the same turn, the dispatcher
   aborts the turn with *"Agent appears stuck (quality monitor exceeded
   its 2-correction cap this turn)."* -- shared 2-cap with the S6.13
   reasoning watchdog.
5. Settings: `WorkspaceConfig::quality_monitor_enabled = false` disables
   the detector globally.

**Expected outcome**: the detector fires only when the loop is genuine
(same tool, same args), at most twice per turn. **Skip-acceptable** if
you can't reproduce a loop in 3-5 minutes -- the unit + integration
tests cover the detector logic deterministically; this manual step is
just a live confirmation.
