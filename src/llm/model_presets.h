#pragma once

#include "llm_client.h"

#include <string>
#include <vector>

namespace locus {

// S4.V Task 6 -- static known-good defaults for the three local-LLM backends
// we test against (LM Studio, Ollama, llama.cpp's `llama-server`) crossed with
// the major model families. The point is purely ergonomic: pick a preset,
// the relevant LLM-tab fields snap to sensible values, the user still types
// the actual model id (every install has different specific files on disk).
//
// Presets are static metadata baked into the binary -- NOT auto-detected
// from a running server. Auto-detection would make S4.N's `ToolFormat::Auto`
// path slightly redundant; instead we let `Auto` cover the unknown-model
// case at runtime and use presets only when the user wants to commit to
// a specific tool format up front (e.g. pinning Qwen XML extraction off,
// or skipping tool calls entirely on a base / pre-tool-trained model).
//
// Fields the preset touches:
//   - base_url:    backend endpoint (the only thing that differs across
//                  LM Studio / Ollama / llama-server)
//   - temperature: family-typical default
//   - max_tokens:  generation cap per response
//   - tool_format: ToolFormat hint matching the family's wire-level habits
//
// Fields the preset deliberately does NOT touch:
//   - model:         every install has a different specific model id;
//                    users type that themselves.
//   - context_limit: usually fine on auto-detect from `/v1/models` (the
//                    `0` sentinel value handles this).
struct ModelPreset {
    std::string name;          // shown in the dropdown
    std::string description;   // hint line under the dropdown
    std::string base_url;
    double      temperature = 0.7;
    int         max_tokens  = 8192;
    ToolFormat  tool_format = ToolFormat::Auto;

    // S4.V Task 7 -- optional sampler defaults. 0 keeps the request body
    // lean and lets the server's per-model default apply. Presets only
    // populate these when the family's published recommendations call for
    // a specific non-default value (e.g. Qwen 2.5's 0.05 min_p, Llama 3's
    // 1.1 repeat_penalty); otherwise they stay at 0.
    double      top_p          = 0.0;
    int         top_k          = 0;
    double      min_p          = 0.0;
    double      repeat_penalty = 0.0;

    // S6.10 Task D -- server-side grammar-constrained decoding for tool calls.
    // Default Off keeps the request body lean and matches pre-S6.10 behaviour.
    // Presets opt in (BestEffort) for families whose servers are known to
    // honour `response_format` json_schema (LM Studio Qwen / Gemma / Llama 3
    // setups, llama-server in general, vLLM).
    GrammarMode grammar_mode  = GrammarMode::Off;
};

// Returns the built-in preset table. Order is the order the dropdown shows.
const std::vector<ModelPreset>& builtin_presets();

// Convenience: find a preset by exact name. Returns nullptr when not found.
const ModelPreset* find_preset(const std::string& name);

// S6.10 Task F -- best-effort match of a server-reported model id to a known
// preset. Case-insensitive regex over the lower-cased id. Returns the first
// matching preset in dropdown order; nullptr on no match (caller keeps the
// current config). The matcher is intentionally conservative: it only
// recognises model families with curated presets, so any model whose family
// isn't represented falls through cleanly.
const ModelPreset* find_preset_for_model(const std::string& model_id);

} // namespace locus
