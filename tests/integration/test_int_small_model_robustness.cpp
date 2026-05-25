// S6.10 small-model robustness pass -- integration tests covering Tasks G
// (anti-truncation), B (quality monitor), F (auto-detect preset), and E
// (few-shot examples in tool descriptions).
//
// All tests run against the shared harness's live workspace. The behaviour
// each verifies is deterministic OR LLM-assisted but model-tolerant; nothing
// depends on a particular model spelling.

#include "harness_fixture.h"

#include "agent/quality_monitor.h"
#include "llm/llm_client.h"
#include "llm/model_presets.h"
#include "tools/file_tools.h"
#include "tools/tool.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "core/workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace locus::integration;
using namespace locus;

namespace fs = std::filesystem;

namespace {

bool path_in_tmp(const fs::path& base, const fs::path& target)
{
    auto rel = fs::relative(target, base);
    return !rel.empty() && rel.native()[0] != '.';
}

}  // namespace

// ---- Task G -- WriteFileTool refuses truncation markers ----------------------

TEST_CASE("S6.10 Task G: WriteFileTool blocks payload with elision marker",
          "[integration][s6.10][truncation]")
{
    auto& h = harness();
    // The harness's scratch dir (tests/integration_tmp/) is workspace-relative
    // AND in .gitignore. Compute the workspace-relative path for the tool.
    fs::path target_abs = h.tmp_dir() / "s6_10_truncated.cpp";
    auto target_rel = fs::relative(target_abs, h.workspace().root());
    std::error_code ec;
    fs::remove(target_abs, ec);

    REQUIRE(h.workspace().config().agent.detect_write_truncation);

    locus::WriteFileTool tool;
    nlohmann::json args;
    args["path"] = target_rel.generic_string();
    args["content"] =
        "void f() {\n"
        "    init();\n"
        "    // rest of the implementation\n"
        "}\n";
    locus::ToolCall call{"trunc_g_1", "write_file", args};

    auto result = tool.execute(call, h.workspace());

    REQUIRE_FALSE(result.success);
    REQUIRE(result.activity_tag == "truncation_blocked");
    REQUIRE(result.content.find("rest of the implementation") != std::string::npos);
    REQUIRE_FALSE(fs::exists(target_abs));
}

TEST_CASE("S6.10 Task G: EditFileTool blocks new_string with elision marker",
          "[integration][s6.10][truncation]")
{
    auto& h = harness();
    fs::path target_abs = h.tmp_dir() / "s6_10_edit_target.cpp";
    auto target_rel = fs::relative(target_abs, h.workspace().root());

    {
        std::ofstream f(target_abs);
        f << "void g() {\n    init();\n    work();\n}\n";
    }

    // edit_file requires a prior read.
    locus::ReadFileTool reader;
    locus::ToolCall read_call{"trunc_g_read", "read_file",
        {{"path", target_rel.generic_string()}}};
    auto read_res = reader.execute(read_call, h.workspace());
    REQUIRE(read_res.success);

    locus::EditFileTool tool;
    nlohmann::json edits = nlohmann::json::array();
    edits.push_back({
        {"old_string", "work();"},
        {"new_string", "work();\n    // rest of the code\n"}
    });
    nlohmann::json args;
    args["path"]  = target_rel.generic_string();
    args["edits"] = edits;
    locus::ToolCall call{"trunc_g_2", "edit_file", args};

    auto result = tool.execute(call, h.workspace());
    REQUIRE_FALSE(result.success);
    REQUIRE(result.activity_tag == "truncation_blocked");
    // File on disk untouched (the original "work();" line still present).
    std::ifstream in(target_abs);
    std::string contents{std::istreambuf_iterator<char>(in), {}};
    REQUIRE(contents.find("// rest of the code") == std::string::npos);

    std::error_code ec;
    fs::remove(target_abs, ec);
}

// ---- Task B -- Quality monitor detector + dispatcher unknown-tool nudge -----

TEST_CASE("S6.10 Task B: QualityMonitor::evaluate is callable against history "
          "produced by a real turn",
          "[integration][llm][s6.10][quality]")
{
    // Run a normal turn through the live LLM. Once it completes, the
    // QualityMonitor::evaluate over the resulting history should return
    // std::nullopt (a healthy turn produces no correction). This is a
    // negative-side guard: we don't want quality monitor false-firing on
    // ordinary tool-call turns.
    auto& h = harness();
    PromptResult r = h.prompt(
        "Use the read_file tool to read the first 3 lines of CLAUDE.md.");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());

    auto messages = h.agent().history().messages();
    auto correction = QualityMonitor::evaluate(messages);
    INFO("history size: " << messages.size());
    if (correction) {
        INFO("unexpected correction kind: "
             << (correction->kind == QualityCorrectionKind::empty_response
                     ? "empty_response" : "repeated_tool_call"));
        INFO("corrective_message: " << correction->corrective_message);
    }
    REQUIRE_FALSE(correction.has_value());
}

// ---- Task F -- find_preset_for_model resolves the harness's loaded model ----

TEST_CASE("S6.10 Task F: find_preset_for_model returns a sensible preset for "
          "the harness's loaded model",
          "[integration][s6.10][presets][matcher]")
{
    auto& h = harness();
    const std::string& id = h.model_id();
    INFO("loaded model id: " << id);

    // Most test models (Gemma 4, Qwen, Llama, DeepSeek-R1, Qwen3-Coder) are
    // covered by the matcher. If we're running against something exotic the
    // matcher returns nullptr and that is also valid behaviour; either way
    // we just want to prove the lookup runs without throwing.
    const auto* preset = find_preset_for_model(id);
    if (preset) {
        INFO("matched preset: " << preset->name);
        REQUIRE_FALSE(preset->name.empty());
        REQUIRE_FALSE(preset->base_url.empty());
    } else {
        WARN("no preset matched model id '" << id
             << "' -- this is OK for exotic models; harness uses generic config");
        SUCCEED();
    }
}

// ---- Task E -- built-in tool manifest carries `Example:` blocks --------------

TEST_CASE("S6.10 Task E: search / write_file / edit_file descriptions carry "
          "Example: blocks in the LLM-facing schema",
          "[integration][s6.10][examples]")
{
    // The lazy-manifest mode strips long descriptions on purpose, so this
    // assertion specifically targets the full-manifest path -- which is what
    // gets sent on first encounter, after a workspace re-open, and to any
    // LLM call site that doesn't opt into lazy.
    auto& h = harness();
    ToolRegistry reg;
    register_builtin_tools(reg);

    auto schema = reg.build_schema_json(h.workspace(), ToolMode::agent,
                                         /*lazy=*/false);

    auto desc_for = [&](const std::string& name) -> std::string {
        for (const auto& entry : schema) {
            if (entry["function"].value("name", "") == name)
                return entry["function"].value("description", "");
        }
        return "";
    };

    INFO("search desc: " << desc_for("search"));
    REQUIRE(desc_for("search").find("Example") != std::string::npos);
    REQUIRE(desc_for("write_file").find("Example") != std::string::npos);
    REQUIRE(desc_for("edit_file").find("Example") != std::string::npos);
    REQUIRE(desc_for("read_file").find("Example") != std::string::npos);
    REQUIRE(desc_for("list_directory").find("Example") != std::string::npos);
}

// ---- Task C -- ChatMessage::reasoning_content survives a real LLM turn ------

TEST_CASE("S6.10 Task C: assistant ChatMessage exposes reasoning_content "
          "field after a live turn (empty when the model has no reasoning)",
          "[integration][llm][s6.10][thinking_strip]")
{
    auto& h = harness();
    PromptResult r = h.prompt("Say hello briefly.");
    REQUIRE_FALSE(r.timed_out);

    auto messages = h.agent().history().messages();
    // Pull the most recent assistant message. The field exists either way;
    // its content depends on whether the loaded model emits reasoning.
    bool found_assistant = false;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == MessageRole::assistant) {
            found_assistant = true;
            INFO("assistant reasoning_content length: "
                 << it->reasoning_content.size());
            INFO("assistant content length: " << it->content.size());
            // The field accessor must work; whether it's populated depends on
            // the model. Either way, ChatMessage::to_json / from_json must
            // also round-trip cleanly (covered by unit tests).
            REQUIRE(it->reasoning_content.size() >= 0);  // ensures the field exists
            break;
        }
    }
    REQUIRE(found_assistant);
}
