#pragma once

#include "llm/llm_client.h"  // ToolSchema

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace locus {

// S6.10 Task D -- helpers for server-side grammar-constrained decoding of
// tool calls. Generates the JSON Schema that LM Studio / llama-server / vLLM
// accept in the `response_format` field to force the model to emit a valid
// tool-call shape at sampling time.
//
// Today only the OpenAI-native path is supported -- the schema describes the
// shape of the model's `content` field as an OpenAI-format tool_calls array.
// llama.cpp's raw `grammar` field (GBNF) plus the Qwen-XML / Claude-XML
// dialects are documented future work and tracked in the S6.10 stage doc.

// Build the union-of-tool-calls JSON Schema for `response_format.json_schema`.
// Shape of the produced schema (matches the OpenAI / LM Studio Structured
// Output spec):
//
//   {
//     "name":   "tool_calls",
//     "strict": true,
//     "schema": {
//       "type": "object",
//       "properties": {
//         "tool_calls": {
//           "type": "array",
//           "minItems": 1,
//           "items": {
//             "oneOf": [
//               { "type": "object",
//                 "properties": {
//                   "name":      { "const": "<tool1>" },
//                   "arguments": { ...tool1 parameters schema... }
//                 },
//                 "required": ["name", "arguments"]
//               },
//               ...
//             ]
//           }
//         }
//       },
//       "required": ["tool_calls"]
//     }
//   }
//
// Empty `tools` returns `nlohmann::json()` (null) -- callers should NOT attach
// `response_format` to the request in that case.
nlohmann::json build_tool_call_union_schema(const std::vector<ToolSchema>& tools);

// Decide whether the current config wants `response_format` attached to the
// request. Returns true when:
//   - grammar_mode != Off
//   - tool_format != None (None tool format means "no tool array sent", so
//     constraining tool calls is meaningless)
//   - `tools` is non-empty (no tools = nothing to constrain)
bool grammar_should_attach(GrammarMode mode, ToolFormat fmt, std::size_t tool_count);

}  // namespace locus
