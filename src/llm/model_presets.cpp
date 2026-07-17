#include "model_presets.h"

#include <algorithm>
#include <regex>

namespace locus {

namespace {

// Three backends crossed with the families we actively test against in
// integration tests. The LM Studio entries dominate the table because LM
// Studio is the developer's primary test surface (see CLAUDE.md "Test
// model" row); Ollama and llama-server are documented escape hatches.
//
// Temperature defaults follow each family's published recommendations:
//   - Gemma / Llama 3.x / generic: 0.7
//   - Qwen 2.5 / 3 (instruct):     0.7 with tool calls (Qwen3 doc nudges
//                                  0.6 but 0.7 has been stable in our suite)
//   - "None" tool-format presets:  same temp; just signals "do not send
//                                  the tools array" for non-tool-trained
//                                  base models.
//
// Order: LM Studio first (most users), then Ollama, then llama-server.
const std::vector<ModelPreset>& presets_table()
{
    // Sampler values follow each family's published recommendations where
    // they differ from the server's defaults:
    //   - Qwen 2.5 / 3:  top_p 0.8, top_k 20, min_p 0.05 (Qwen docs)
    //   - Llama 3.x:     repeat_penalty 1.1 keeps long codegen on track
    //                    (Meta's published guidance + suggestions in the
    //                    llama.cpp issue tracker for tool-heavy turns)
    //   - Gemma family:  no per-family override -- server defaults are fine
    //   - Base / Ollama / llama-server generic: same, defaults stay 0
    //
    // Anything left at 0 means "preset doesn't override the server".
    // S6.10 Task D -- per-family grammar_mode defaults. BestEffort wins for
    // LM Studio (Structured Output is documented and stable) and llama-server
    // (raw GBNF + response_format both supported); Off for non-tool-trained
    // base models (grammar makes no sense there) and Ollama (response_format
    // support is partial / behind `format: json` rather than full json_schema).
    // S6.10 Task H additions:
    //   - LM Studio -- Qwen3-Coder      (separate from generic Qwen; temp=0.1)
    //   - LM Studio -- DeepSeek-R1-Distill (temp=0.6 top_p=0.95, reasoning-heavy)
    //   - LM Studio -- Gemma family     gains top_k=64
    //   - LM Studio -- Llama 3.x family gains top_p=0.9
    static const std::vector<ModelPreset> k_presets = {
        {
            "LM Studio -- Gemma family",
            "Gemma 2 / 3 / 4 instruct via LM Studio. Auto tool-format covers Gemma's OpenAI-style tool_calls cleanly. top_k=64 follows Gemma's published recommendation.",
            "http://127.0.0.1:1234",
            0.7, 8192, ToolFormat::Auto,
            0.0, 64, 0.0, 0.0,
            GrammarMode::BestEffort,
        },
        {
            "LM Studio -- Qwen family",
            "Qwen 2.5 / 3 instruct via LM Studio. Pins Qwen XML extraction so <tool_call> markers in the text channel are recovered if LM Studio's chat template lets them through. Samplers follow Qwen's published thinking-mode recommendations (temp 0.6, top_p 0.95) -- hybrid Qwen 3.x models default to thinking, where the non-thinking values (0.7 / 0.8) drive quantized models into degenerate repetition loops.",
            "http://127.0.0.1:1234",
            0.6, 8192, ToolFormat::Qwen,
            0.95, 20, 0.0, 0.0,
            GrammarMode::BestEffort,
        },
        {
            "LM Studio -- Qwen3-Coder",
            "Qwen3-Coder-7B / 14B / 32B-Instruct via LM Studio. Coder-specific defaults: temperature 0.1, top_p 0.9 (Qwen3-Coder card recommends low-temperature for code).",
            "http://127.0.0.1:1234",
            0.1, 8192, ToolFormat::Auto,
            0.9, 0, 0.0, 0.0,
            GrammarMode::BestEffort,
        },
        {
            "LM Studio -- DeepSeek-R1-Distill",
            "DeepSeek-R1-Distill-Qwen / Llama variants via LM Studio. Reasoning-heavy distills want temperature 0.6 and top_p 0.95 per the model card.",
            "http://127.0.0.1:1234",
            0.6, 8192, ToolFormat::Auto,
            0.95, 0, 0.0, 0.0,
            GrammarMode::BestEffort,
        },
        {
            "LM Studio -- Llama 3.x family",
            "Llama 3 / 3.1 / 3.3 instruct via LM Studio. Auto tool-format handles both the native JSON path and any Claude-style markers some fine-tunes emit. top_p=0.9 and repeat_penalty=1.1 keep long codegen turns on track.",
            "http://127.0.0.1:1234",
            0.7, 8192, ToolFormat::Auto,
            0.9, 0, 0.0, 1.1,
            GrammarMode::BestEffort,
        },
        {
            "LM Studio -- base / non-tool-trained",
            "For models without tool-call training (raw Llama / Mistral base, narrative models). Skips the tools array entirely so the model isn't pushed toward malformed function calls.",
            "http://127.0.0.1:1234",
            0.7, 8192, ToolFormat::None,
            0.0, 0, 0.0, 0.0,
            GrammarMode::Off,
        },
        {
            "Ollama -- generic",
            "Ollama's OpenAI-compatible endpoint at the default port 11434. Auto tool-format works across Ollama's bundled Gemma / Qwen / Llama families.",
            "http://127.0.0.1:11434",
            0.7, 8192, ToolFormat::Auto,
            0.0, 0, 0.0, 0.0,
            GrammarMode::Off,
        },
        {
            "llama-server (llama.cpp) -- generic",
            "llama.cpp's built-in HTTP server at the default port 8080. Auto tool-format; max_tokens raised slightly because llama-server doesn't apply a soft cap.",
            "http://127.0.0.1:8080",
            0.7, 8192, ToolFormat::Auto,
            0.0, 0, 0.0, 0.0,
            GrammarMode::BestEffort,
        },
    };
    return k_presets;
}

} // namespace

const std::vector<ModelPreset>& builtin_presets()
{
    return presets_table();
}

const ModelPreset* find_preset(const std::string& name)
{
    for (const auto& p : presets_table()) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const ModelPreset* find_preset_for_model(const std::string& model_id)
{
    if (model_id.empty()) return nullptr;

    // Fold the id to lower-case so the regex patterns can be written as
    // plain literal substrings (no \i flag in std::regex). Strip the LM
    // Studio "publisher/" prefix LM Studio prepends for catalog entries
    // (e.g. "google/gemma-4-e4b" -> "gemma-4-e4b") so the patterns hit.
    std::string id = model_id;
    if (auto slash = id.find('/'); slash != std::string::npos)
        id = id.substr(slash + 1);
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Match order matters -- more-specific patterns come first so the
    // Qwen3-Coder preset wins over the generic Qwen preset, and the
    // DeepSeek-R1-Distill preset wins over the generic Llama preset.
    // Each entry: (regex pattern, preset name in builtin_presets()).
    static const std::pair<std::regex, const char*> k_rules[] = {
        {std::regex(R"(qwen.?3.*coder)"),         "LM Studio -- Qwen3-Coder"},
        {std::regex(R"(deepseek.*r1)"),           "LM Studio -- DeepSeek-R1-Distill"},
        {std::regex(R"(qwen.?(2\.5|3))"),         "LM Studio -- Qwen family"},
        {std::regex(R"(gemma.?(2|3|4))"),         "LM Studio -- Gemma family"},
        {std::regex(R"((llama.?3|meta.*llama.?3))"), "LM Studio -- Llama 3.x family"},
    };

    for (const auto& rule : k_rules) {
        if (std::regex_search(id, rule.first)) {
            return find_preset(rule.second);
        }
    }
    return nullptr;
}

} // namespace locus
