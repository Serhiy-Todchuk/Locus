# Claude Native API Support (Anthropic `/v1/messages`)

> **Status: Backlog.** Follow-up to [S6.16 -- Endpoint Profiles](../M6/S6.16-endpoint-profiles.md), which shipped Claude support via OpenAI-compat proxy (LiteLLM, `claude-code-proxy`, etc.). This draft covers native Anthropic API support -- direct `/v1/messages` calls, no proxy.

## Why this is parked

S6.16 deliberately chose the OpenAI-compat proxy route because it lands "config + UI" sized work instead of "config + UI + new transport + new SSE decoder + new tool-call shape + new persistence migration" sized work. The proxy path covers the user-visible use case (chat with Claude from the chat-panel chip) at zero new transport / decoder cost.

Reasons against doing native today:

1. **The proxy works.** LiteLLM ships, is maintained, and translates OpenAI -> Anthropic faithfully enough that Locus's existing stream decoder + tool-call extractor handle the round-trip without code changes. The user only pays the cost of running one local process they already would have run for other tooling (Aider, Cursor's hosted-eval workflows, etc.).
2. **The native protocol is genuinely different.** It is not "OpenAI with different field names":
   - Endpoint shape: `/v1/messages` (not `/v1/chat/completions`), `system` is a top-level field (not a message role), `max_tokens` is required.
   - Auth: `x-api-key` header + `anthropic-version: 2023-06-01` header (not `Authorization: Bearer`).
   - Tool calls live in `content` blocks as `tool_use` items (not in a `tool_calls` array). `tool_result` is a content block in a `user` message, not a `tool`-role message.
   - SSE events are semantic, not chunked deltas: `message_start`, `content_block_start`, `content_block_delta` (with sub-types `text_delta` / `input_json_delta` / `thinking_delta`), `content_block_stop`, `message_delta`, `message_stop`. None of the existing `OpenAiDecoder` / `QwenXmlDecoder` / `ClaudeXmlDecoder` shapes apply.
   - Reasoning ("extended thinking") arrives as `thinking` content blocks with a signed payload that must be echoed back verbatim on the next turn or Anthropic refuses the request -- new persistence wrinkle.
3. **None of this is hard.** It is "small new client class + small new SSE decoder + small message-shape mapper." Maybe ~600-1000 LOC including tests. The bar isn't difficulty -- it's *necessity*, and the proxy makes necessity an empirical question.

## What lands when reactivated

If the trigger fires, the work shape is:

- **`src/llm/anthropic_client.h/cpp`** -- new `AnthropicClient : ILLMClient`. Composes a new `AnthropicTransport` (own cpr POST against `/v1/messages` + own SSE event-name parser; the existing `OpenAiTransport::SseParser` only knows `data: ` lines, which Anthropic uses too, but the event names matter -- decoder must dispatch on them) + a new `AnthropicStreamDecoder` implementing `IStreamDecoder`.
- **`src/llm/anthropic_message_mapper.h/cpp`** -- pure function pair `to_anthropic_request(messages, tools, system_prompt)` and `from_anthropic_event(event, delta)`. Maps:
  - `MessageRole::system` -> top-level `system` field, removed from `messages[]`.
  - `MessageRole::tool` -> `user` message with a `tool_result` content block.
  - `assistant` with `tool_calls[]` -> `assistant` with one `tool_use` content block per call.
  - Plain text + reasoning -> `content[]` array of `text` + `thinking` blocks.
- **Tool schema shape.** Anthropic uses a slightly different schema -- `input_schema` instead of `parameters`, no `function` wrapper. The mapper handles it; `ToolSchema` doesn't change.
- **`EndpointProfile` extension.** New optional `provider_kind` enum (`OpenAiCompat | Anthropic`); when `Anthropic`, the factory creates `AnthropicClient` instead of `LMStudioClient`. Default profile shape stays OpenAI-compat; Anthropic is opt-in per profile.
- **Reasoning persistence.** `ChatMessage::reasoning_content` already exists ([S6.10 Task C](../M6/S6.10-small-model-robustness.md)). Anthropic's signed thinking blocks need a parallel `reasoning_signature` field (opaque string the model returned, which must be passed back verbatim). Adds one optional JSON field to the wire and to `to_json` / `from_json`.
- **Tests.** Catch2: round-trip mapper, signed-thinking persistence, tool_use parsing, `tool_result` injection. Integration test: live API call (manual-only, needs ANTHROPIC_API_KEY).
- **Settings.** Endpoint editor (already shipped by S6.16) gains a `Provider:` dropdown (`OpenAI-compat` / `Anthropic`). No new settings panel.

Estimated scope: ~1-1.5 weeks of focused work on top of S6.16.

## Reactivation triggers

Reactivate when **any** of these is true:

- The OpenAI-compat proxy story breaks for a real workflow -- a specific Claude feature (multi-image content, computer-use tool, extended-thinking signing) doesn't survive the round-trip.
- Real workloads show users on Locus + Claude refusing to run a proxy (deployment friction outweighs the implementation cost).
- A second native-protocol provider lands on the same wish list (Gemini -- not OpenAI-compatible by default; OpenAI Responses API -- partly different shape), in which case the right move is one stage that introduces the `provider_kind` abstraction and ships two native clients at once.

Until one of those triggers fires, the OpenAI-compat proxy is the right answer.

## Companion to

- [S6.16 -- Endpoint Profiles](../M6/S6.16-endpoint-profiles.md) -- ships the multi-endpoint scaffolding this would consume. S6.16 explicitly leaves the door open: "Anthropic native is **not** in scope. [...] A separate stage can introduce `AnthropicClient : ILLMClient` + a native SSE decoder if proxies prove insufficient."
- [S4.Q -- Strong/Weak Model Split](S4.Q-strong-weak-model.md) -- different concern (model role routing) but shares the `EndpointProfileStore` substrate; a future stage covering both would be coherent.
