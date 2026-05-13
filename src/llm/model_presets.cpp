#include "model_presets.h"

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
    static const std::vector<ModelPreset> k_presets = {
        {
            "LM Studio -- Gemma family",
            "Gemma 2 / 3 / 4 instruct via LM Studio. Auto tool-format covers Gemma's OpenAI-style tool_calls cleanly.",
            "http://127.0.0.1:1234",
            0.7, 8192, ToolFormat::Auto,
        },
        {
            "LM Studio -- Qwen family",
            "Qwen 2.5 / 3 instruct via LM Studio. Pins Qwen XML extraction so <tool_call> markers in the text channel are recovered if LM Studio's chat template lets them through.",
            "http://127.0.0.1:1234",
            0.7, 8192, ToolFormat::Qwen,
        },
        {
            "LM Studio -- Llama 3.x family",
            "Llama 3 / 3.1 / 3.3 instruct via LM Studio. Auto tool-format handles both the native JSON path and any Claude-style markers some fine-tunes emit.",
            "http://127.0.0.1:1234",
            0.7, 8192, ToolFormat::Auto,
        },
        {
            "LM Studio -- base / non-tool-trained",
            "For models without tool-call training (raw Llama / Mistral base, narrative models). Skips the tools array entirely so the model isn't pushed toward malformed function calls.",
            "http://127.0.0.1:1234",
            0.7, 8192, ToolFormat::None,
        },
        {
            "Ollama -- generic",
            "Ollama's OpenAI-compatible endpoint at the default port 11434. Auto tool-format works across Ollama's bundled Gemma / Qwen / Llama families.",
            "http://127.0.0.1:11434",
            0.7, 8192, ToolFormat::Auto,
        },
        {
            "llama-server (llama.cpp) -- generic",
            "llama.cpp's built-in HTTP server at the default port 8080. Auto tool-format; max_tokens raised slightly because llama-server doesn't apply a soft cap.",
            "http://127.0.0.1:8080",
            0.7, 8192, ToolFormat::Auto,
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

} // namespace locus
