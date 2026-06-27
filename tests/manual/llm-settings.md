# LLM Settings -- Thinking ON/OFF/auto (S6.14)

Verifies the Settings -> LLM "Thinking" dropdown forces the model's
chain-of-thought reasoning on or off at the source, per model family. The knob
is applied at workspace open (like endpoint/model), so each toggle needs a
restart to take effect -- the dialog reminds you with the standard "Restart
Locus to apply" prompt.

Reasoning is only visible when the loaded model has a reasoning channel. Tests 1
and 2 need a Qwen 3.x hybrid model loaded in LM Studio (e.g. `qwen3-14b`,
`qwen3.6-27b`). Test 3 needs any non-Qwen, non-o1, non-DeepSeek-R1 model
(e.g. a Gemma or Llama instruct).

## Test 1 -- Off suppresses the Thoughts bubble (Qwen 3.x)

1. With a Qwen 3.x model loaded, open Settings (Ctrl+,) -> LLM tab. Confirm
   "Thinking" shows **Auto**.
2. Send a prompt that makes the model reason, e.g. "Two trains 300 km apart head
   toward each other at 60 and 40 km/h. When do they meet? Think step by step."
   - Expected: a collapsible **Thoughts** bubble appears above the answer.
3. Open Settings -> LLM, set Thinking to **Off**, click OK. Accept the "Restart
   Locus to apply" prompt, then quit and relaunch the same workspace.
4. Send the same prompt again.
   - Expected: **no Thoughts bubble** -- the model answers directly. The answer
     itself is still correct (5 km apart... it meets at 3 h).
5. Quit and open `.locus/locus.log`. Find the `LLM client:` startup line.
   - Expected: it ends with `thinking=off`.

   Hard fail: a Thoughts bubble still renders after the relaunch, or the log
   shows `thinking=auto` -- the config didn't round-trip or the injection didn't
   fire.

## Test 2 -- On brings the Thoughts bubble back

1. Continuing from Test 1 (Thinking is Off), open Settings -> LLM, set Thinking
   to **On**, OK, accept the restart prompt, relaunch.
2. Send the reasoning prompt from Test 1 step 2.
   - Expected: the **Thoughts** bubble returns. The startup log line now ends
     with `thinking=on`.

## Test 3 -- Unknown family logs a warning and changes nothing

1. Load a non-Qwen, non-o1, non-DeepSeek-R1 model (e.g. a Gemma or Llama
   instruct) in LM Studio.
2. Open Settings -> LLM, set Thinking to **Off**, OK, accept the restart prompt,
   relaunch.
3. Send any prompt and let it answer normally.
4. Quit and open `.locus/locus.log`.
   - Expected: a single warn line like
     `thinking_mode forced to Off but no known mechanism for model '<id>';
     ignoring`. The model behaves exactly as it would with Thinking=Auto -- the
     unknown-family path is a no-op, so nothing about the response changes.

   Hard fail: the warning repeats every turn (it must fire at most once per
   model id per process), or the response is malformed -- the no-op path must
   never alter the request for an unrecognised family.
