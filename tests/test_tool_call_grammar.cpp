// S6.10 Task D -- grammar-constrained decoding helper tests.
//
// Covers:
// - GrammarMode <-> string round-trip
// - build_tool_call_union_schema: shape, single-tool fast path, multi-tool
//   union (oneOf), empty input
// - grammar_should_attach: gate combinations
// - ModelPreset round-trips grammar_mode field

#include "llm/tool_call_grammar.h"
#include "llm/llm_client.h"
#include "llm/model_presets.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

using locus::build_tool_call_union_schema;
using locus::find_preset;
using locus::GrammarMode;
using locus::grammar_mode_from_string;
using locus::grammar_should_attach;
using locus::ToolFormat;
using locus::ToolSchema;
using locus::to_string;
using nlohmann::json;

TEST_CASE("grammar_mode string conversion round-trips", "[s6.10][grammar]")
{
    REQUIRE(grammar_mode_from_string("off")         == GrammarMode::Off);
    REQUIRE(grammar_mode_from_string("best_effort") == GrammarMode::BestEffort);
    REQUIRE(grammar_mode_from_string("best-effort") == GrammarMode::BestEffort);
    REQUIRE(grammar_mode_from_string("strict")      == GrammarMode::Strict);
    REQUIRE(grammar_mode_from_string("BEST_EFFORT") == GrammarMode::BestEffort);
    REQUIRE(grammar_mode_from_string("force")       == GrammarMode::Strict);
    // Unknown / empty / typo all default safely to Off.
    REQUIRE(grammar_mode_from_string("")            == GrammarMode::Off);
    REQUIRE(grammar_mode_from_string("typo")        == GrammarMode::Off);

    // Round-trip
    REQUIRE(std::string(to_string(GrammarMode::Off))        == "off");
    REQUIRE(std::string(to_string(GrammarMode::BestEffort)) == "best_effort");
    REQUIRE(std::string(to_string(GrammarMode::Strict))     == "strict");
}

TEST_CASE("build_tool_call_union_schema: empty input returns null",
          "[s6.10][grammar]")
{
    auto schema = build_tool_call_union_schema({});
    REQUIRE(schema.is_null());
}

TEST_CASE("build_tool_call_union_schema: single-tool schema is well-formed",
          "[s6.10][grammar]")
{
    ToolSchema t;
    t.name        = "read_file";
    t.description = "Read a file";
    t.parameters  = json{
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}}},
        }},
        {"required", json::array({"path"})},
    };

    auto wrapper = build_tool_call_union_schema({t});
    REQUIRE(wrapper.is_object());
    REQUIRE(wrapper["name"]   == "tool_calls");
    REQUIRE(wrapper["strict"] == true);

    auto& schema = wrapper["schema"];
    REQUIRE(schema["type"] == "object");
    REQUIRE(schema["required"][0] == "tool_calls");

    auto& tc = schema["properties"]["tool_calls"];
    REQUIRE(tc["type"]     == "array");
    REQUIRE(tc["minItems"] == 1);

    // Single-tool fast path: items is the variant directly, not a oneOf.
    auto& items = tc["items"];
    REQUIRE(items.contains("properties"));
    REQUIRE(items["properties"]["name"]["const"] == "read_file");
    REQUIRE(items["properties"]["arguments"]["properties"].contains("path"));
    REQUIRE(items["required"][0] == "name");
}

TEST_CASE("build_tool_call_union_schema: multi-tool produces oneOf union",
          "[s6.10][grammar]")
{
    ToolSchema a;
    a.name        = "read_file";
    a.parameters  = json{{"type", "object"}, {"properties", json::object()}};

    ToolSchema b;
    b.name        = "write_file";
    b.parameters  = json{{"type", "object"}, {"properties", json::object()}};

    ToolSchema c;
    c.name        = "list_directory";
    c.parameters  = json{{"type", "object"}, {"properties", json::object()}};

    auto wrapper = build_tool_call_union_schema({a, b, c});
    auto& items  = wrapper["schema"]["properties"]["tool_calls"]["items"];
    REQUIRE(items.contains("oneOf"));
    REQUIRE(items["oneOf"].size() == 3);

    // Each variant carries the right name const.
    REQUIRE(items["oneOf"][0]["properties"]["name"]["const"] == "read_file");
    REQUIRE(items["oneOf"][1]["properties"]["name"]["const"] == "write_file");
    REQUIRE(items["oneOf"][2]["properties"]["name"]["const"] == "list_directory");
}

TEST_CASE("build_tool_call_union_schema: empty tool params get permissive object",
          "[s6.10][grammar]")
{
    ToolSchema t;
    t.name        = "no_args_tool";
    t.parameters  = json::object();  // empty schema

    auto wrapper = build_tool_call_union_schema({t});
    auto& items  = wrapper["schema"]["properties"]["tool_calls"]["items"];
    REQUIRE(items["properties"]["arguments"]["type"] == "object");
    REQUIRE(items["properties"]["arguments"]["additionalProperties"] == true);
}

TEST_CASE("grammar_should_attach: gate combinations", "[s6.10][grammar]")
{
    // Off never attaches.
    REQUIRE_FALSE(grammar_should_attach(GrammarMode::Off, ToolFormat::OpenAi, 5));
    REQUIRE_FALSE(grammar_should_attach(GrammarMode::Off, ToolFormat::Auto,   5));

    // None tool format means no tools array sent -- nothing to constrain.
    REQUIRE_FALSE(grammar_should_attach(GrammarMode::BestEffort, ToolFormat::None, 5));

    // Zero tools likewise.
    REQUIRE_FALSE(grammar_should_attach(GrammarMode::BestEffort, ToolFormat::OpenAi, 0));

    // Happy paths.
    REQUIRE(grammar_should_attach(GrammarMode::BestEffort, ToolFormat::OpenAi, 3));
    REQUIRE(grammar_should_attach(GrammarMode::BestEffort, ToolFormat::Auto,   3));
    REQUIRE(grammar_should_attach(GrammarMode::BestEffort, ToolFormat::Qwen,   3));
    REQUIRE(grammar_should_attach(GrammarMode::Strict,     ToolFormat::OpenAi, 3));
}

TEST_CASE("ModelPreset carries grammar_mode", "[s6.10][grammar][model_presets]")
{
    // LM Studio family presets opt in to BestEffort; the base / non-tool-trained
    // preset stays Off because constraining tool calls is meaningless when the
    // tools array isn't even sent. Ollama is Off because Structured Output
    // support is partial behind `format: json`.
    auto* gemma = find_preset("LM Studio -- Gemma family");
    REQUIRE(gemma != nullptr);
    REQUIRE(gemma->grammar_mode == GrammarMode::BestEffort);

    auto* qwen = find_preset("LM Studio -- Qwen family");
    REQUIRE(qwen != nullptr);
    REQUIRE(qwen->grammar_mode == GrammarMode::BestEffort);

    auto* llama = find_preset("LM Studio -- Llama 3.x family");
    REQUIRE(llama != nullptr);
    REQUIRE(llama->grammar_mode == GrammarMode::BestEffort);

    auto* base = find_preset("LM Studio -- base / non-tool-trained");
    REQUIRE(base != nullptr);
    REQUIRE(base->grammar_mode == GrammarMode::Off);

    auto* ollama = find_preset("Ollama -- generic");
    REQUIRE(ollama != nullptr);
    REQUIRE(ollama->grammar_mode == GrammarMode::Off);

    auto* llama_srv = find_preset("llama-server (llama.cpp) -- generic");
    REQUIRE(llama_srv != nullptr);
    REQUIRE(llama_srv->grammar_mode == GrammarMode::BestEffort);
}
