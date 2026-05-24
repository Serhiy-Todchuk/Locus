# run_command output filter

Default smart-truncate + per-call filter modes (`head_tail` / `head` / `tail` / `regex` / `substring`) that trim verbose `run_command` and `read_process_output` output before it reaches the LLM. Full raw output goes to `.locus/locus.log` at trace level (`-verbose`) so nothing is lost.

## Test 1 -- default head_tail truncation + Settings spinner

1. Open Settings > Tool Approvals. The spinner labelled "run_command output: keep first + last N lines" reads 50.
2. Set it to 20. Click OK. Open `.locus/config.json`: `agent.run_command_truncate_lines` is 20.
3. Ask the agent to run a verbose command. Easiest repro: `cmake --build build/release --config Release --target locus_tests` (one full release build). On the resulting `msg-tool` bubble the body opens with `[exit code: 0]` and shows ~20 lines from the top, the elision marker `[... N lines elided; full output in .locus/locus.log (trace level) ...]`, then ~20 lines from the tail.
4. Set the spinner back to 50 and OK. Re-run the same command. The body now has the wider head+tail window. Run a *short* command (e.g. `git status`) -- no elision marker (the body fits inside `2 * lines` and the helper short-circuits to the unmodified output).
5. Set the spinner to 0 and OK. Re-run the verbose command. The body returns raw (no marker, no head/tail trimming). The 1 MB hard backstop still applies; truly huge outputs end with `[... raw output exceeded 1 MB; remainder discarded ...]`.

**Expected outcome**: spinner round-trips through `.locus/config.json`, the chat bubble shows the right slice for each setting, and the elision marker carries the recovery hint.

## Test 2 -- explicit per-call filter modes

1. Set the spinner back to a reasonable default (50). Ask the agent to deliberately use the inline filter: *"Run a release build and use output_filter_mode='regex' with output_filter_pattern='error|warning|fatal' and output_filter_lines=20 and output_filter_context=2."*
2. The chat bubble's body starts with `[output_filter regex: K of N lines matched]` and shows only the matching lines (with two leading + two trailing context lines per match). The body is much shorter than the default head_tail would have produced.
3. Repeat with `output_filter_mode='tail'`, `output_filter_lines=10`. The body shows only the last 10 lines + the elision marker for the earlier content.
4. Repeat with `output_filter_mode='head'`, `output_filter_lines=10`. Mirror of the tail case (top instead of bottom).
5. Repeat with `output_filter_mode='substring'`, `output_filter_pattern='error C'` (no regex metacharacters) -- only lines literally containing `error C` come back.

**Expected outcome**: each mode trims as advertised; the filter summary line names the mode and the match count.

## Test 3 -- raw recovery via trace log

1. Launch Locus from a console with `-verbose`. Drive any `run_command` whose output exceeds the spinner setting so it gets filtered.
2. Open `.locus/locus.log`. Search for `run_command: exit=` lines. The matching block is followed by `--- raw ---\n<full body>\n--- end raw ---`. The complete pre-filter output is there byte-for-byte (subject to the 1 MB hard backstop).
3. Also try with a background process: `run_command_bg` to start a chatty process (`cmake --build ...`), then `read_process_output` while it's still building. The chat-side bubble is filtered; the locus.log has the matching `read_process_output: id=... --- raw --- ...` block with the unfiltered window. **Hard fail** if the log shows only the filtered text.

**Expected outcome**: trace log is the always-on recovery path; the user can grep / scroll the raw build output without re-running the command.
