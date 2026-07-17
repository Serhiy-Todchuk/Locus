// S4.V Task 6 -- model-card preset table. Pure-data validation: the table is
// static metadata baked into the binary, so the tests just check shape and
// that the lookup behaves the way the Settings dialog expects.

#include <catch2/catch_test_macros.hpp>

#include "llm/model_presets.h"

using locus::builtin_presets;
using locus::find_preset;
using locus::ToolFormat;

TEST_CASE("builtin_presets returns a non-empty table", "[s4.v][presets]")
{
    const auto& t = builtin_presets();
    REQUIRE_FALSE(t.empty());
}

TEST_CASE("every preset has a name, description, and endpoint",
          "[s4.v][presets]")
{
    for (const auto& p : builtin_presets()) {
        INFO("preset: " << p.name);
        REQUIRE_FALSE(p.name.empty());
        REQUIRE_FALSE(p.description.empty());
        REQUIRE_FALSE(p.base_url.empty());
        REQUIRE(p.temperature >= 0.0);
        REQUIRE(p.temperature <= 2.0);
        REQUIRE(p.max_tokens >= 256);
    }
}

TEST_CASE("preset names are unique", "[s4.v][presets]")
{
    const auto& t = builtin_presets();
    for (size_t i = 0; i < t.size(); ++i) {
        for (size_t j = i + 1; j < t.size(); ++j) {
            INFO("collision: '" << t[i].name << "' vs '" << t[j].name << "'");
            REQUIRE(t[i].name != t[j].name);
        }
    }
}

TEST_CASE("find_preset returns the right entry or null",
          "[s4.v][presets]")
{
    const auto& t = builtin_presets();
    REQUIRE(t.size() >= 1);

    // First entry round-trips.
    const auto* p = find_preset(t.front().name);
    REQUIRE(p != nullptr);
    REQUIRE(p->base_url == t.front().base_url);

    // Unknown name returns nullptr.
    REQUIRE(find_preset("Does Not Exist") == nullptr);
    REQUIRE(find_preset("") == nullptr);
}

TEST_CASE("all three backends are represented", "[s4.v][presets]")
{
    // The whole point of the feature is "LM Studio / Ollama / llama-server".
    // If any of those backends drops out of the table we've broken the
    // contract.
    bool lmstudio = false, ollama = false, llamaserver = false;
    for (const auto& p : builtin_presets()) {
        if (p.base_url == "http://127.0.0.1:1234")  lmstudio    = true;
        if (p.base_url == "http://127.0.0.1:11434") ollama      = true;
        if (p.base_url == "http://127.0.0.1:8080")  llamaserver = true;
    }
    REQUIRE(lmstudio);
    REQUIRE(ollama);
    REQUIRE(llamaserver);
}

TEST_CASE("Qwen family preset pins ToolFormat::Qwen", "[s4.v][presets]")
{
    // Qwen 2.5 / 3 family preset's wire-level habit is `<tool_call>...` XML
    // markers. If our table ever stops pinning Qwen the XML extraction is
    // silently dropped to Auto, which still works but is suboptimal -- guard
    // against regression on the "family" preset specifically. S6.10 Task H
    // added a Qwen3-Coder preset that pins Auto (Qwen3-Coder's tool calls
    // come through cleanly via LM Studio's native JSON path), so this guard
    // is on the family preset only.
    const auto* p = find_preset("LM Studio -- Qwen family");
    REQUIRE(p != nullptr);
    REQUIRE(p->tool_format == ToolFormat::Qwen);
}

TEST_CASE("non-tool-trained preset pins ToolFormat::None",
          "[s4.v][presets]")
{
    bool found = false;
    for (const auto& p : builtin_presets()) {
        if (p.tool_format == ToolFormat::None) {
            found = true;
            REQUIRE_FALSE(p.description.empty());
        }
    }
    REQUIRE(found);
}

TEST_CASE("sampler defaults: Qwen preset carries Qwen's published values",
          "[s4.v][presets][samplers]")
{
    // Retuned 2026-07-17 to Qwen's THINKING-mode recommendations (temp 0.6,
    // top_p 0.95, no min_p) after the agentic gl_cube run: hybrid Qwen 3.x
    // models default to thinking, where the old non-thinking values
    // (0.7 / 0.8 / min_p 0.05) drove quantized models into degenerate
    // repetition loops that blew the max_tokens cap before a tool call.
    // If the table drifts again, retune deliberately -- this guard catches
    // the accident.
    const locus::ModelPreset* p = nullptr;
    for (const auto& q : builtin_presets()) {
        if (q.name.find("Qwen") != std::string::npos) { p = &q; break; }
    }
    REQUIRE(p != nullptr);
    REQUIRE(p->temperature == 0.6);
    REQUIRE(p->top_p == 0.95);
    REQUIRE(p->top_k == 20);
    REQUIRE(p->min_p == 0.0);
    REQUIRE(p->repeat_penalty == 0.0);  // Qwen doesn't recommend one
}

TEST_CASE("sampler defaults: Llama 3.x preset suggests repeat_penalty 1.1 and top_p 0.9",
          "[s4.v][s6.10][presets][samplers]")
{
    // S6.10 Task H -- top_p 0.9 added to keep long codegen turns on track
    // alongside the previously-present repeat_penalty 1.1.
    const locus::ModelPreset* p = nullptr;
    for (const auto& q : builtin_presets()) {
        if (q.name.find("Llama 3") != std::string::npos) { p = &q; break; }
    }
    REQUIRE(p != nullptr);
    REQUIRE(p->repeat_penalty == 1.1);
    REQUIRE(p->top_p == 0.9);
}

TEST_CASE("sampler defaults: Gemma preset gains top_k 64 (S6.10 Task H)",
          "[s6.10][presets][samplers]")
{
    const locus::ModelPreset* p = nullptr;
    for (const auto& q : builtin_presets()) {
        if (q.name.find("Gemma") != std::string::npos) { p = &q; break; }
    }
    REQUIRE(p != nullptr);
    REQUIRE(p->top_k == 64);
}

TEST_CASE("sampler defaults: Qwen3-Coder preset (S6.10 Task H)",
          "[s6.10][presets][samplers]")
{
    const auto* p = find_preset("LM Studio -- Qwen3-Coder");
    REQUIRE(p != nullptr);
    REQUIRE(p->temperature == 0.1);
    REQUIRE(p->top_p == 0.9);
}

TEST_CASE("sampler defaults: DeepSeek-R1-Distill preset (S6.10 Task H)",
          "[s6.10][presets][samplers]")
{
    const auto* p = find_preset("LM Studio -- DeepSeek-R1-Distill");
    REQUIRE(p != nullptr);
    REQUIRE(p->temperature == 0.6);
    REQUIRE(p->top_p == 0.95);
}

TEST_CASE("sampler defaults: presets without overrides stay at 0",
          "[s4.v][presets][samplers]")
{
    // The Ollama-generic / llama-server-generic / non-tool-trained presets
    // are deliberately silent on samplers so the server's per-model defaults
    // apply. If a future edit accidentally fills one of them in, we want to
    // know. (Gemma / Llama 3.x carry post-S6.10 tuning -- see their dedicated
    // tests above.)
    for (const auto& p : builtin_presets()) {
        if (p.name.find("Ollama")    != std::string::npos ||
            p.name.find("llama-server") != std::string::npos ||
            p.name.find("base /")    != std::string::npos)
        {
            INFO("preset: " << p.name);
            REQUIRE(p.top_p          == 0.0);
            REQUIRE(p.top_k          == 0);
            REQUIRE(p.min_p          == 0.0);
            REQUIRE(p.repeat_penalty == 0.0);
        }
    }
}

// -- S6.10 Task F -- model id -> preset matcher ------------------------------

TEST_CASE("find_preset_for_model: empty id returns nullptr",
          "[s6.10][presets][matcher]")
{
    REQUIRE(locus::find_preset_for_model("") == nullptr);
}

TEST_CASE("find_preset_for_model: unknown model id returns nullptr",
          "[s6.10][presets][matcher]")
{
    REQUIRE(locus::find_preset_for_model("totally-made-up-model") == nullptr);
    REQUIRE(locus::find_preset_for_model("falcon-180b") == nullptr);
}

TEST_CASE("find_preset_for_model: Qwen3-Coder beats generic Qwen",
          "[s6.10][presets][matcher]")
{
    const auto* p = locus::find_preset_for_model("qwen3-coder-7b-instruct-q4_k_m");
    REQUIRE(p != nullptr);
    REQUIRE(p->name == "LM Studio -- Qwen3-Coder");
}

TEST_CASE("find_preset_for_model: generic Qwen2.5/3 matches the Qwen family preset",
          "[s6.10][presets][matcher]")
{
    const auto* p = locus::find_preset_for_model("Qwen3-32B-Instruct-Q4_K_M");
    REQUIRE(p != nullptr);
    REQUIRE(p->name == "LM Studio -- Qwen family");

    const auto* q = locus::find_preset_for_model("qwen2.5-14b-instruct");
    REQUIRE(q != nullptr);
    REQUIRE(q->name == "LM Studio -- Qwen family");
}

TEST_CASE("find_preset_for_model: Gemma 2/3/4 ids match",
          "[s6.10][presets][matcher]")
{
    for (const auto* id : {
            "gemma-2-9b-it", "gemma-3-1b-it", "gemma-4-e4b",
            "google/gemma-4-9b-it"
        })
    {
        INFO("id: " << id);
        const auto* p = locus::find_preset_for_model(id);
        REQUIRE(p != nullptr);
        REQUIRE(p->name == "LM Studio -- Gemma family");
    }
}

TEST_CASE("find_preset_for_model: DeepSeek-R1 distills match dedicated preset",
          "[s6.10][presets][matcher]")
{
    for (const auto* id : {
            "deepseek-r1-distill-qwen-32b",
            "deepseek-r1-distill-llama-8b",
            "DeepSeek-R1-Distill-Qwen-1.5B",
        })
    {
        INFO("id: " << id);
        const auto* p = locus::find_preset_for_model(id);
        REQUIRE(p != nullptr);
        REQUIRE(p->name == "LM Studio -- DeepSeek-R1-Distill");
    }
}

TEST_CASE("find_preset_for_model: Llama 3.x ids match",
          "[s6.10][presets][matcher]")
{
    for (const auto* id : {
            "meta-llama-3.1-8b-instruct",
            "Llama-3.3-70B-Instruct-Q4_K_M",
            "meta-llama/Llama-3-8b-Instruct",
        })
    {
        INFO("id: " << id);
        const auto* p = locus::find_preset_for_model(id);
        REQUIRE(p != nullptr);
        REQUIRE(p->name == "LM Studio -- Llama 3.x family");
    }
}
