#pragma once

// S6.14 -- thinking ON/OFF/auto request augmentation.
//
// Locus already handles reasoning content on the READ side (the stream decoder
// routes `reasoning_content` to a separate DOM channel; Layer 3 compaction
// strips leaked <think>...</think> blocks). This helper is the WRITE side: it
// turns thinking off (or on) AT THE SOURCE for the next request, per the loaded
// model's family.
//
// The mechanism differs by family because there is no single cross-model switch:
//
//   Qwen 3.x hybrid (qwen3.6-*, qwen3-thinking-*):
//       request["chat_template_kwargs"]["enable_thinking"] = false / true
//   Qwen 2.x / older qwen-* :
//       append "\n/no_think" (off) or "\n/think" (on) to the last user message
//   o1-style / DeepSeek-R1 (o1-*, deepseek-r1-*):
//       request["reasoning_effort"] = "low" (off) / "high" (on)
//   Unknown family:
//       no-op, warn-logged once per process
//
// ThinkingMode::Auto always no-ops -- the server-default behaviour runs.
//
// S4.F invariant: the Qwen 2.x mechanism mutates `messages[last_user]` content
// per request. That breaks byte-stable prefix-cache reuse on the LAST user
// message but NOT on messages[0] (system) -- thinking-mode toggles are explicit
// user actions, so preserving KV cache here is out of scope.

#include <nlohmann/json.hpp>

#include <string_view>
#include <vector>

namespace locus {

struct ChatMessage;  // llm/llm_client.h
enum class ThinkingMode;  // llm/llm_client.h

// Family the model id resolves to for thinking-injection purposes. Public so
// the unit tests can assert the classifier directly.
enum class ThinkingFamily {
    Qwen3Hybrid,   // chat_template_kwargs.enable_thinking
    Qwen2Legacy,   // /no_think /think message suffix
    ReasoningEffort,  // o1 / deepseek-r1: reasoning_effort low/high
    Unknown,       // no known mechanism -> no-op + warn
};

// Pure classifier. Lower-cases the id, strips a "publisher/" prefix (LM Studio
// catalog ids), then matches family substrings. Match order is most-specific
// first so qwen3-coder / qwen3-thinking land on Qwen3Hybrid before the legacy
// qwen rule, and deepseek-r1 lands on ReasoningEffort before any qwen rule.
ThinkingFamily classify_thinking_family(std::string_view model_id);

// Augment `request` (and possibly `messages`) so the NEXT completion runs with
// thinking forced on/off per the loaded model's family. `request` is the JSON
// body assembled by build_request_body BEFORE serialise; `messages` is the same
// vector the body was built from (the Qwen 2.x path edits the last user message
// in place, so the caller must rebuild body["messages"] from `messages` after
// this returns -- or pass `messages` in before the body's "messages" key is
// populated).
//
// Auto -> no-op. Unknown family + non-Auto -> no-op + one warn per process.
void apply_thinking_mode(nlohmann::json&           request,
                         std::vector<ChatMessage>& messages,
                         ThinkingMode              mode,
                         std::string_view          model_id);

} // namespace locus
