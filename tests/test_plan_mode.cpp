// S4.D -- plan-mode unit tests.
//
// Covers:
//   - AgentMode enum + string conversion round-trip
//   - Plan / PlanStep helpers (finished_count, all_done, plan_to_json)
//   - propose_plan tool: happy path + every validation error
//   - mark_step_done tool: happy path + every validation error
//   - Tool catalog filtering by ToolMode (plan / execute / agent)
//
// AgentCore-level tests (mode transitions, plan_awaiting_decision_ gating)
// would need a live LLM and a TurnDriver -- they belong in integration tests
// (a future [plan] tag), not here.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "agent/agent_mode.h"
#include "tools/plan_tools.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "support/fake_workspace_services.h"

#include <filesystem>

namespace fs = std::filesystem;
using namespace locus;
using json = nlohmann::json;

namespace {

ToolCall make_call(const std::string& tool_name, json args)
{
    ToolCall c;
    c.id = "call_test";
    c.tool_name = tool_name;
    c.args = std::move(args);
    return c;
}

ToolResult run(ITool& tool, json args)
{
    auto tmp = fs::temp_directory_path() / "locus_test_plan_tools";
    fs::create_directories(tmp);
    test::FakeWorkspaceServices ws(tmp);
    return tool.execute(make_call(tool.name(), std::move(args)), ws);
}

} // namespace

// ===========================================================================
// AgentMode round-trip
// ===========================================================================

TEST_CASE("AgentMode: to_string / agent_mode_from_string round-trip",
          "[s4.d][agent-mode]")
{
    for (auto m : {AgentMode::chat, AgentMode::plan, AgentMode::execute}) {
        CHECK(agent_mode_from_string(to_string(m)) == m);
    }
    // Unknown / empty strings fall back to chat.
    CHECK(agent_mode_from_string("garbage") == AgentMode::chat);
    CHECK(agent_mode_from_string("")        == AgentMode::chat);
}

// ===========================================================================
// Plan / PlanStep helpers
// ===========================================================================

TEST_CASE("Plan: finished_count / all_done track step status",
          "[s4.d][plan]")
{
    Plan p;
    p.id = "plan-1";
    p.steps.push_back({"a", {}, PlanStep::Status::pending, ""});
    p.steps.push_back({"b", {}, PlanStep::Status::pending, ""});
    p.steps.push_back({"c", {}, PlanStep::Status::pending, ""});

    CHECK(p.finished_count() == 0);
    CHECK_FALSE(p.all_done());

    p.steps[0].status = PlanStep::Status::done;
    CHECK(p.finished_count() == 1);
    CHECK_FALSE(p.all_done());

    p.steps[1].status = PlanStep::Status::failed;  // failed counts as finished
    p.steps[2].status = PlanStep::Status::done;
    CHECK(p.finished_count() == 3);
    CHECK(p.all_done());
}

TEST_CASE("Plan: empty plan never reports all_done", "[s4.d][plan]")
{
    Plan p;
    p.id = "plan-empty";
    CHECK_FALSE(p.all_done());
    CHECK(p.finished_count() == 0);
}

TEST_CASE("Plan: plan_to_json round-trip preserves structure",
          "[s4.d][plan]")
{
    Plan p;
    p.id = "plan-7";
    p.title = "Refactor renderer";
    p.summary = "Split renderer into pipeline + buffers.";
    p.steps.push_back({"Extract buffers", {"edit_file"},
                        PlanStep::Status::pending, ""});
    p.steps.push_back({"Rewrite pipeline", {"edit_file", "read_file"},
                        PlanStep::Status::done, "all green"});

    json j = plan_to_json(p);
    CHECK(j["id"]      == "plan-7");
    CHECK(j["title"]   == "Refactor renderer");
    CHECK(j["summary"] == "Split renderer into pipeline + buffers.");
    REQUIRE(j["steps"].is_array());
    REQUIRE(j["steps"].size() == 2);
    CHECK(j["steps"][0]["description"]  == "Extract buffers");
    CHECK(j["steps"][0]["status"]       == "pending");
    CHECK(j["steps"][0]["tools_needed"][0] == "edit_file");
    CHECK(j["steps"][1]["status"]       == "done");
    CHECK(j["steps"][1]["notes"]        == "all green");
}

// ===========================================================================
// propose_plan tool
// ===========================================================================

TEST_CASE("propose_plan: visible only in plan mode", "[s4.d][propose-plan]")
{
    ProposePlanTool t;
    CHECK(t.visible_in_mode(ToolMode::plan));
    CHECK_FALSE(t.visible_in_mode(ToolMode::agent));
    CHECK_FALSE(t.visible_in_mode(ToolMode::execute));
    CHECK_FALSE(t.visible_in_mode(ToolMode::subagent));
}

TEST_CASE("propose_plan: happy path echoes the plan as ok=true JSON",
          "[s4.d][propose-plan]")
{
    ProposePlanTool t;
    json args = {
        {"title",   "Build cornell box"},
        {"summary", "Three files"},
        {"steps",   json::array({
            json{{"description", "Create index.html"}, {"tools_needed", json::array({"write_file"})}},
            json{{"description", "Create main.js"}}
        })}
    };
    auto r = run(t, args);
    REQUIRE(r.success);

    auto j = json::parse(r.content);
    CHECK(j["ok"]      == true);
    CHECK(j["title"]   == "Build cornell box");
    CHECK(j["summary"] == "Three files");
    REQUIRE(j["steps"].size() == 2);
    CHECK(j["steps"][0]["description"]  == "Create index.html");
    CHECK(j["steps"][0]["tools_needed"][0] == "write_file");
    // missing tools_needed in step 2 normalizes to empty array
    CHECK(j["steps"][1]["tools_needed"].is_array());
    CHECK(j["steps"][1]["tools_needed"].empty());
}

TEST_CASE("propose_plan: missing 'steps' fails", "[s4.d][propose-plan]")
{
    ProposePlanTool t;
    auto r = run(t, json{{"title", "x"}});
    REQUIRE_FALSE(r.success);
    auto j = json::parse(r.content);
    CHECK(j["ok"] == false);
    CHECK(j["error"].get<std::string>().find("steps") != std::string::npos);
}

TEST_CASE("propose_plan: empty 'steps' fails", "[s4.d][propose-plan]")
{
    ProposePlanTool t;
    auto r = run(t, json{{"steps", json::array()}});
    REQUIRE_FALSE(r.success);
    auto j = json::parse(r.content);
    CHECK(j["ok"] == false);
    CHECK(j["error"].get<std::string>().find("empty") != std::string::npos);
}

TEST_CASE("propose_plan: step without description fails",
          "[s4.d][propose-plan]")
{
    ProposePlanTool t;
    auto r = run(t, json{{"steps",
                          json::array({json{{"tools_needed", json::array()}}})}});
    REQUIRE_FALSE(r.success);
    auto j = json::parse(r.content);
    CHECK(j["error"].get<std::string>().find("description") != std::string::npos);
}

TEST_CASE("propose_plan: step is not an object fails",
          "[s4.d][propose-plan]")
{
    ProposePlanTool t;
    auto r = run(t, json{{"steps", json::array({"just a string"})}});
    REQUIRE_FALSE(r.success);
    auto j = json::parse(r.content);
    CHECK(j["error"].get<std::string>().find("not an object") != std::string::npos);
}

TEST_CASE("propose_plan: preview text reflects step count",
          "[s4.d][propose-plan]")
{
    ProposePlanTool t;
    auto call = make_call("propose_plan", json{
        {"title", "tiny"},
        {"steps", json::array({
            json{{"description", "a"}}, json{{"description", "b"}}
        })}
    });
    auto p = t.preview(call);
    CHECK(p.find("tiny")    != std::string::npos);
    CHECK(p.find("2 steps") != std::string::npos);
}

// ===========================================================================
// mark_step_done tool
// ===========================================================================

TEST_CASE("mark_step_done: visible only in execute mode",
          "[s4.d][mark-step-done]")
{
    MarkStepDoneTool t;
    CHECK(t.visible_in_mode(ToolMode::execute));
    CHECK_FALSE(t.visible_in_mode(ToolMode::agent));
    CHECK_FALSE(t.visible_in_mode(ToolMode::plan));
    CHECK_FALSE(t.visible_in_mode(ToolMode::subagent));
}

TEST_CASE("mark_step_done: happy path emits ok=true with step+status",
          "[s4.d][mark-step-done]")
{
    MarkStepDoneTool t;
    auto r = run(t, json{
        {"step", 2}, {"status", "done"}, {"notes", "all good"}
    });
    REQUIRE(r.success);
    auto j = json::parse(r.content);
    CHECK(j["ok"]     == true);
    CHECK(j["step"]   == 2);
    CHECK(j["status"] == "done");
    CHECK(j["notes"]  == "all good");
}

TEST_CASE("mark_step_done: defaults status to done when omitted",
          "[s4.d][mark-step-done]")
{
    MarkStepDoneTool t;
    auto r = run(t, json{{"step", 1}});
    REQUIRE(r.success);
    auto j = json::parse(r.content);
    CHECK(j["status"] == "done");
    CHECK_FALSE(j.contains("notes"));  // notes optional
}

TEST_CASE("mark_step_done: missing 'step' fails",
          "[s4.d][mark-step-done]")
{
    MarkStepDoneTool t;
    auto r = run(t, json{});
    REQUIRE_FALSE(r.success);
    auto j = json::parse(r.content);
    CHECK(j["error"].get<std::string>().find("step") != std::string::npos);
}

TEST_CASE("mark_step_done: non-integer 'step' fails",
          "[s4.d][mark-step-done]")
{
    MarkStepDoneTool t;
    auto r = run(t, json{{"step", "first"}});
    REQUIRE_FALSE(r.success);
    auto j = json::parse(r.content);
    CHECK(j["error"].get<std::string>().find("integer") != std::string::npos);
}

TEST_CASE("mark_step_done: zero or negative 'step' fails",
          "[s4.d][mark-step-done]")
{
    MarkStepDoneTool t;
    {
        auto r = run(t, json{{"step", 0}});
        REQUIRE_FALSE(r.success);
    }
    {
        auto r = run(t, json{{"step", -1}});
        REQUIRE_FALSE(r.success);
    }
}

TEST_CASE("mark_step_done: invalid 'status' fails",
          "[s4.d][mark-step-done]")
{
    MarkStepDoneTool t;
    auto r = run(t, json{{"step", 1}, {"status", "skipped"}});
    REQUIRE_FALSE(r.success);
    auto j = json::parse(r.content);
    CHECK(j["error"].get<std::string>().find("done") != std::string::npos);
}

// ===========================================================================
// Tool catalog filtering by mode
// ===========================================================================

TEST_CASE("ToolRegistry: agent mode hides plan-mode tools",
          "[s4.d][tool-catalog]")
{
    ToolRegistry reg;
    register_builtin_tools(reg);

    auto tmp = fs::temp_directory_path() / "locus_test_plan_catalog";
    fs::create_directories(tmp);
    test::FakeWorkspaceServices ws(tmp);

    auto agent_schema = reg.build_schema_json(ws, ToolMode::agent);
    bool saw_propose = false, saw_mark = false;
    for (auto& e : agent_schema) {
        if (!e.contains("function")) continue;
        std::string name = e["function"].value("name", "");
        if (name == "propose_plan")    saw_propose = true;
        if (name == "mark_step_done")  saw_mark    = true;
    }
    CHECK_FALSE(saw_propose);
    CHECK_FALSE(saw_mark);
}

TEST_CASE("ToolRegistry: plan mode exposes only propose_plan",
          "[s4.d][tool-catalog]")
{
    ToolRegistry reg;
    register_builtin_tools(reg);

    auto tmp = fs::temp_directory_path() / "locus_test_plan_catalog2";
    fs::create_directories(tmp);
    test::FakeWorkspaceServices ws(tmp);

    auto schema = reg.build_schema_json(ws, ToolMode::plan);

    // Exactly one tool, and that tool is propose_plan.
    REQUIRE(schema.size() == 1);
    REQUIRE(schema[0].contains("function"));
    CHECK(schema[0]["function"].value("name", "") == "propose_plan");
}

TEST_CASE("ToolRegistry: execute mode exposes mark_step_done plus the catalog",
          "[s4.d][tool-catalog]")
{
    ToolRegistry reg;
    register_builtin_tools(reg);

    auto tmp = fs::temp_directory_path() / "locus_test_plan_catalog3";
    fs::create_directories(tmp);
    test::FakeWorkspaceServices ws(tmp);

    auto schema = reg.build_schema_json(ws, ToolMode::execute);

    bool saw_mark = false, saw_propose = false, saw_read = false;
    for (auto& e : schema) {
        if (!e.contains("function")) continue;
        std::string name = e["function"].value("name", "");
        if (name == "mark_step_done") saw_mark    = true;
        if (name == "propose_plan")   saw_propose = true;
        if (name == "read_file")      saw_read    = true;
    }
    CHECK(saw_mark);
    CHECK_FALSE(saw_propose);   // plan-mode-only
    CHECK(saw_read);            // standard catalog flows through
}
