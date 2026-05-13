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

TEST_CASE("Qwen preset pins ToolFormat::Qwen", "[s4.v][presets]")
{
    // Qwen's wire-level habit is `<tool_call>...` XML markers. If our table
    // ever stops pinning Qwen the XML extraction is silently dropped to Auto,
    // which still works but is suboptimal -- guard against regression.
    bool found = false;
    for (const auto& p : builtin_presets()) {
        if (p.name.find("Qwen") != std::string::npos) {
            REQUIRE(p.tool_format == ToolFormat::Qwen);
            found = true;
        }
    }
    REQUIRE(found);
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
    // Qwen 2.5 / 3 docs recommend top_p 0.8, top_k 20, min_p 0.05. If the
    // table ever drifts from those, retune deliberately rather than by
    // accident -- this guard catches the accident.
    const locus::ModelPreset* p = nullptr;
    for (const auto& q : builtin_presets()) {
        if (q.name.find("Qwen") != std::string::npos) { p = &q; break; }
    }
    REQUIRE(p != nullptr);
    REQUIRE(p->top_p == 0.8);
    REQUIRE(p->top_k == 20);
    REQUIRE(p->min_p == 0.05);
    REQUIRE(p->repeat_penalty == 0.0);  // Qwen doesn't recommend one
}

TEST_CASE("sampler defaults: Llama 3.x preset suggests repeat_penalty 1.1",
          "[s4.v][presets][samplers]")
{
    const locus::ModelPreset* p = nullptr;
    for (const auto& q : builtin_presets()) {
        if (q.name.find("Llama 3") != std::string::npos) { p = &q; break; }
    }
    REQUIRE(p != nullptr);
    REQUIRE(p->repeat_penalty == 1.1);
}

TEST_CASE("sampler defaults: presets without overrides stay at 0",
          "[s4.v][presets][samplers]")
{
    // The Gemma / Ollama-generic / llama-server-generic / non-tool-trained
    // presets are deliberately silent on samplers so the server's per-model
    // defaults apply. If a future edit accidentally fills one of them in,
    // we want to know.
    for (const auto& p : builtin_presets()) {
        if (p.name.find("Gemma")     != std::string::npos ||
            p.name.find("Ollama")    != std::string::npos ||
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
