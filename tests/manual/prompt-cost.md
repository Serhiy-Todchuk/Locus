# Prompt Cost (S6.11 + S6.12)

Two paired Settings -- `Lazy tool manifest` (checkbox) and `System prompt profile` (Full / Compact / Minimal dropdown) -- that trim the per-turn fixed overhead so 16k-context local LLMs have room for actual conversation. Lives on Settings > Tool Approvals under the run_command head+tail spinner. ADR-0007 has the cross-cutting rationale; this plan exercises both knobs end-to-end through the UI + the `system prompt ~N tokens (profile=X)` log line.

## Test 1 -- both knobs round-trip through Settings + config.json

1. Open Settings > Tool Approvals. Defaults: lazy manifest unchecked, profile dropdown reads `Full (default, ~700t)`.
2. Tick lazy manifest. Switch profile to `Compact (~300t)`. Click OK.
3. Open `.locus/config.json` -- `agent.lazy_tool_manifest` is `true` and `agent.system_prompt_profile` is `"compact"`.
4. Quit Locus. Relaunch on the same workspace. Reopen Settings > Tool Approvals -- both controls still reflect the new state.
5. Switch profile to `Minimal (~80t, power users)`, untick lazy manifest, OK. Quit, relaunch. Settings now show `Minimal` + unchecked.
6. Edit `.locus/config.json` by hand: set `system_prompt_profile` to `"bogus"`. Relaunch -- the dropdown falls back to `Full` and the log shows `Unknown system_prompt.profile 'bogus'; defaulting to 'full'`.

**Expected outcome**: both knobs persist via the workspace config, and an invalid string falls back silently to Full with a warn-log.

## Test 2 -- token-cost log lines change as expected

1. Launch Locus with `-verbose` from a console. Open the log tail (`Get-Content -Wait .locus/locus.log` or similar).
2. With **Full + lazy=off** (defaults), send any message. Note the line `AgentCore: system prompt ~N tokens (profile=full), context limit M`. Record N -- typical ~3500 with the default 12-tool roster.
3. Open Settings, switch profile to **Compact**, OK. Send another message. The same log line now reads `(profile=compact)` and N drops by a few hundred tokens (the Rules / Editing / Shell prose shrinks).
4. Tick **Lazy tool manifest** and OK. Send another message. The `Tool manifest: 12 tools, ~N tokens` line drops sharply (~2854 -> ~400). The next `system prompt ~N tokens` line also shrinks because the prose "## Available Tools" section collapses to one-liners.
5. Switch profile to **Minimal**, leave lazy on, OK, send again. The prompt is now at the minimum -- N should be well under 1500 tokens.

**Expected outcome**: log lines name the active profile and the manifest token count tracks both knobs. Each toggle is a one-time prompt-cache invalidation; the next turn's `cached=` should be `0` (LM Studio rebuilds the prefix), and the turn after that should hit cache again.

## Test 3 -- describe_tool fetches the full schema when lazy mode is on

1. Settings: lazy manifest **on**, profile any. OK.
2. Prompt: *"Call `describe_tool` for the name `edit_file` and tell me the parameters."* (Or wait for the agent to call it itself on a turn that uses an unfamiliar tool.)
3. Observe in chat: a `describe_tool` tool-call bubble, then its result contains the full OpenAI-format JSON schema for `edit_file` (including `edits` array shape, `path`, `replace_all`, etc.).
4. Prompt: *"Call `describe_tool` for the name `bogus_tool`."* Result is a `success=false` bubble with text like `"tool 'bogus_tool' is not registered. Did you mean '...'? Available tools: ..."`.
5. Toggle lazy manifest **off**, OK, send any prompt. The `Tool manifest:` log line bounces back to the full ~2854 t shape and `describe_tool` is no longer in the per-turn manifest (verify via the activity log's `tool_manifest` event).

**Expected outcome**: `describe_tool` is the on-demand schema fetch when lazy is on; gone from the manifest when lazy is off (but still callable from the tools panel if a power user wants to inspect).
