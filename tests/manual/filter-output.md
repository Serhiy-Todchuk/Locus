# Filter Output Tool (S5.Z task 8)

`filter_output` is a regex / substring filter the agent can apply to text it already has in context. Use case: a previous `run_command` returned 25 KB of build output; instead of asking the LLM to eyeball it, the LLM hands it to `filter_output` with a pattern and gets back only the matching lines.

The tool is auto-approve, categorised as `read` (pure-CPU, no FS access). Verified surfaces:

## Test 1 -- agent-driven filter happy path

1. Open WS1 with the LLM running. In chat, ask: "Run `cmake --build build/release --config Release --target locus_tests` and then filter the output for lines containing `error`. Use the filter_output tool."
2. Expect the agent to:
   - Call `run_command` (approve if it's set to ask).
   - Pass the captured build output to `filter_output` with `pattern: "error"` (regex or substring -- either is fine).
   - Return a summary line (`X of Y lines matched`) plus the matched lines, prefixed with their 1-based line numbers.
3. Switch to the Activity panel. The `filter_output` row carries the matched lines verbatim; no approval gate fires (auto-approve).
4. **Token win check.** The build log might be many KB; the chat shows the trimmed result, not the raw blob. If the build succeeds clean, the summary reads `0 of N lines matched` and no lines follow.

## Test 2 -- context lines + cap + invert

1. Ask the agent: "Filter the previous build output for `warning` with 2 lines of context before each match. Cap at 5 matches."
2. Expect the agent to invoke `filter_output` with `pattern: "warning"`, `before: 2`, `max_matches: 5`. Output uses grep-style: `<line>:<text>` for the match itself and `<line>-<text>` for context lines; non-contiguous blocks separated by `--`.
3. Now: "Filter the same text but show me lines that do NOT contain `warning`." Agent should call `filter_output` with `invert: true`. Expect everything except the warning lines, capped at 50 (default).
4. Hard fail: response exceeds the 50-line default and the summary doesn't say `(capped at N)`.

## Test 3 -- error paths

1. Ask the agent: "Filter the previous output for the pattern `(` -- yes, just an unbalanced paren." Expect the tool to return `Error: invalid regex '(': ...` and `success=false`. The activity row marks it as an error; the LLM should explain and either retry with a fixed pattern or fall back to substring mode.
2. Ask: "Filter the previous output -- substring mode -- for `.cpp`." Substring mode is literal so `.` matches a dot, not "any char". Expect each line containing `.cpp` to come back; lines without it dropped.
3. Hard fail: substring mode treats `.` as regex, returning every line.

**Expected outcome**: the tool round-trips matches with line numbers; invalid regex produces a clean error not a crash; substring mode is literal; `invert` and `before`/`after` work as documented; default cap of 50 is enforced and reported.
