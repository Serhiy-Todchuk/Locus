// S4.X -- prompt template registry tests.
//
// Covers: registry scan + project-wins-on-collision, positional / named
// substitution, frontmatter parsing, mtime cache invalidation, escape
// handling, dispatcher integration (help + completion + AgentCore routing).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "agent/agent_core.h"
#include "agent/prompt_templates.h"
#include "agent/slash_commands.h"
#include "core/workspace.h"
#include "llm/llm_client.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path make_temp_dir(const std::string& tag)
{
    auto p = fs::temp_directory_path() / ("locus_test_s4x_" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

void write_text(const fs::path& path, const std::string& body)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
}

} // namespace

// -- pure substitution -------------------------------------------------------

TEST_CASE("PromptTemplateRegistry::substitute: positional + named + missing",
          "[s4.x][prompt-templates]")
{
    using R = locus::PromptTemplateRegistry;

    std::vector<std::string> positional = {"alpha", "beta"};
    std::unordered_map<std::string, std::string> kwargs = {
        {"name", "Locus"}, {"version", "0.1"}
    };

    // Mixed positional + named, with missing references rendered literal.
    auto out = R::substitute(
        "hello {name}, args: {0}, {1}; missing: {2} {nope}",
        positional, kwargs);
    REQUIRE(out == "hello Locus, args: alpha, beta; missing: {2} {nope}");
}

TEST_CASE("PromptTemplateRegistry::substitute: escape with {{ and }}",
          "[s4.x][prompt-templates]")
{
    using R = locus::PromptTemplateRegistry;

    // `{{name}}` -> literal `{name}` (not a substitution).
    auto out = R::substitute("a {{name}} b {0}",
                             {"X"},
                             {{"name", "Locus"}});
    REQUIRE(out == "a {name} b X");
}

TEST_CASE("PromptTemplateRegistry::substitute: placeholder spanning newline is literal",
          "[s4.x][prompt-templates]")
{
    using R = locus::PromptTemplateRegistry;

    // We deliberately don't allow newlines inside placeholders. A `{` followed
    // by newline before a `}` falls through as literal text.
    auto out = R::substitute("hello {\nworld}", {}, {});
    REQUIRE(out == "hello {\nworld}");
}

// -- frontmatter parsing -----------------------------------------------------

TEST_CASE("parse_file_contents: with frontmatter",
          "[s4.x][prompt-templates]")
{
    std::string src =
        "---\n"
        "description: Generate a standup from this week's git activity\n"
        "args: [target_date, author]\n"
        "---\n"
        "Body line 1\n"
        "Body line 2 with {target_date}\n";

    auto t = locus::PromptTemplateRegistry::parse_file_contents(src, "standup");
    REQUIRE(t.name == "standup");
    REQUIRE(t.description == "Generate a standup from this week's git activity");
    REQUIRE(t.args.size() == 2);
    REQUIRE(t.args[0] == "target_date");
    REQUIRE(t.args[1] == "author");
    REQUIRE_THAT(t.body, ContainsSubstring("Body line 1"));
    REQUIRE_THAT(t.body, ContainsSubstring("{target_date}"));
}

TEST_CASE("parse_file_contents: no frontmatter, whole body kept",
          "[s4.x][prompt-templates]")
{
    std::string src = "Plain template body\nNo frontmatter at all.\n";
    auto t = locus::PromptTemplateRegistry::parse_file_contents(src, "plain");
    REQUIRE(t.description.empty());
    REQUIRE(t.args.empty());
    REQUIRE(t.body == src);
}

// -- registry: scan + project-wins -----------------------------------------

TEST_CASE("PromptTemplateRegistry: scans project + global; project wins on collision",
          "[s4.x][prompt-templates]")
{
    auto root = make_temp_dir("scan");
    auto project = root / "project";
    auto global  = root / "global";

    write_text(project / "review.md",   "PROJECT review {0}");
    write_text(project / "only_proj.md", "only project");
    write_text(global  / "review.md",   "GLOBAL review {0}");
    write_text(global  / "only_glob.md", "only global");

    locus::PromptTemplateRegistry reg(project, global);

    REQUIRE(reg.has("review"));
    REQUIRE(reg.has("only_proj"));
    REQUIRE(reg.has("only_glob"));
    REQUIRE_FALSE(reg.has("does_not_exist"));

    auto entries = reg.list();
    REQUIRE(entries.size() == 3);

    // Project entry for "review" wins.
    auto rv = reg.find("review");
    REQUIRE(rv.has_value());
    REQUIRE(rv->is_project);
    REQUIRE_THAT(rv->body, ContainsSubstring("PROJECT"));

    // Expansion uses the project body.
    auto exp = reg.expand("review", {"X"}, {});
    REQUIRE_THAT(exp, ContainsSubstring("PROJECT review X"));

    std::error_code rm_ec;
    fs::remove_all(root, rm_ec);
}

// -- registry: mtime cache invalidation -----------------------------------

TEST_CASE("PromptTemplateRegistry: detects on-disk edits via mtime check",
          "[s4.x][prompt-templates]")
{
    auto root = make_temp_dir("mtime");
    auto project = root / "project";
    auto global  = root / "global";
    fs::create_directories(global);  // global empty

    write_text(project / "hello.md", "v1 body");
    locus::PromptTemplateRegistry reg(project, global);

    {
        auto t = reg.find("hello");
        REQUIRE(t.has_value());
        REQUIRE(t->body == "v1 body");
    }

    // Filesystem mtime resolution can be 1 s on FAT/NTFS; sleep just over
    // a second so the new write registers a different file_time.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write_text(project / "hello.md", "v2 body");

    {
        auto t = reg.find("hello");
        REQUIRE(t.has_value());
        REQUIRE(t->body == "v2 body");
    }

    std::error_code rm_ec;
    fs::remove_all(root, rm_ec);
}

// -- registry: reload picks up new + removed files -------------------------

TEST_CASE("PromptTemplateRegistry::reload: picks up new files and drops removed",
          "[s4.x][prompt-templates]")
{
    auto root = make_temp_dir("reload");
    auto project = root / "project";
    auto global  = root / "global";
    fs::create_directories(project);
    fs::create_directories(global);

    locus::PromptTemplateRegistry reg(project, global);
    REQUIRE(reg.list().empty());

    write_text(project / "fresh.md", "freshly added");
    REQUIRE(reg.list().empty());          // not picked up yet

    int n = reg.reload();
    REQUIRE(n == 1);
    REQUIRE(reg.has("fresh"));

    fs::remove(project / "fresh.md");
    n = reg.reload();
    REQUIRE(n == 0);
    REQUIRE_FALSE(reg.has("fresh"));

    std::error_code rm_ec;
    fs::remove_all(root, rm_ec);
}

// -- dispatcher: complete() folds templates --------------------------------

TEST_CASE("SlashCommandDispatcher::complete includes templates",
          "[s4.x][prompt-templates][slash]")
{
    auto root = make_temp_dir("complete");
    auto project = root / "project";
    auto global  = root / "global";
    write_text(project / "review_pr.md", "Review pull request");
    fs::create_directories(global);

    locus::PromptTemplateRegistry reg(project, global);

    // Real workspace required so the IWorkspaceServices-typed services arg
    // to SlashCommandDispatcher can resolve `services_.workspace()`.
    auto ws_root = make_temp_dir("complete_ws");
    locus::Workspace ws(ws_root);

    locus::ToolRegistry tools;
    locus::register_builtin_tools(tools);

    locus::SlashCommandDispatcher disp(tools, ws);
    disp.set_template_registry(&reg);

    auto matches = disp.complete("review");
    bool found_template = false;
    for (auto& m : matches) {
        if (m.name == "review_pr") {
            REQUIRE(m.kind == "template");
            REQUIRE_THAT(m.description, ContainsSubstring("Review pull request") ||
                                        ContainsSubstring("(prompt template)"));
            found_template = true;
        }
    }
    REQUIRE(found_template);

    std::error_code rm_ec;
    fs::remove_all(root,    rm_ec);
    fs::remove_all(ws_root, rm_ec);
}

// -- dispatcher: render_help lists templates in their own group ------------

TEST_CASE("SlashCommandDispatcher::try_dispatch /help lists templates",
          "[s4.x][prompt-templates][slash]")
{
    auto root = make_temp_dir("help");
    auto project = root / "project";
    auto global  = root / "global";
    write_text(project / "standup.md",
               "---\n"
               "description: Generate a standup\n"
               "args: [target_date]\n"
               "---\n"
               "Body");
    write_text(global / "ship_changelog.md", "global only");

    locus::PromptTemplateRegistry reg(project, global);

    auto ws_root = make_temp_dir("help_ws");
    locus::Workspace ws(ws_root);

    locus::ToolRegistry tools;
    locus::register_builtin_tools(tools);

    locus::SlashCommandDispatcher disp(tools, ws);
    disp.set_template_registry(&reg);

    std::string out, err;
    bool handled = disp.try_dispatch(
        "/help",
        [&](std::string s) { out += s; },
        [&](std::string s) { err += s; });
    REQUIRE(handled);
    REQUIRE(err.empty());
    REQUIRE_THAT(out, ContainsSubstring("Templates (project)"));
    REQUIRE_THAT(out, ContainsSubstring("standup"));
    REQUIRE_THAT(out, ContainsSubstring("Generate a standup"));
    REQUIRE_THAT(out, ContainsSubstring("Templates (global)"));
    REQUIRE_THAT(out, ContainsSubstring("ship_changelog"));

    std::error_code rm_ec;
    fs::remove_all(root,    rm_ec);
    fs::remove_all(ws_root, rm_ec);
}

// -- AgentCore: a template `/foo` expands into a user message -------------

namespace {

// Stub LLM so we can drive AgentCore without LM Studio. Captures the user
// message that lands in the prompt and returns a no-op completion.
class StubLlm : public locus::ILLMClient {
public:
    locus::ModelInfo query_model_info() override { return {"stub", 8192}; }

    void stream_completion(const std::vector<locus::ChatMessage>& msgs,
                           const std::vector<locus::ToolSchema>& /*tools*/,
                           const locus::StreamCallbacks& cb) override
    {
        // Record the latest user message (the one driving this turn).
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->role == locus::MessageRole::user) {
                last_user_message = it->content;
                break;
            }
        }
        if (cb.on_token) cb.on_token("ok");
        if (cb.on_usage) {
            locus::CompletionUsage u;
            u.prompt_tokens = 10;
            u.completion_tokens = 1;
            cb.on_usage(u);
        }
        if (cb.on_complete) cb.on_complete();
    }

    std::string last_user_message;
};

// Bare-minimum frontend that satisfies the IFrontend pure-virtual surface
// and captures the on_error stream so test cases can assert on it.
class CaptureFrontend : public locus::IFrontend {
public:
    std::string errors;
    std::string tokens;

    void on_turn_start() override {}
    void on_token(std::string_view t) override { tokens.append(t); }
    void on_tool_call_pending(const locus::ToolCall&, const std::string&, bool,
                              const std::vector<std::string>&) override {}
    void on_tool_result(const std::string&, const std::string&) override {}
    void on_turn_complete() override {}
    void on_context_meter(int, int, int, int) override {}
    void on_compaction_needed(int, int) override {}
    void on_session_reset() override {}
    void on_error(const std::string& m) override { errors += m + "\n"; }
    void on_embedding_progress(int, int) override {}
    void on_activity(const locus::ActivityEvent&) override {}
};

} // namespace

TEST_CASE("AgentCore: `/foo` resolves to a prompt template and lands in history",
          "[s4.x][prompt-templates][agent]")
{
    auto root = make_temp_dir("agent_route");
    auto ws_root = make_temp_dir("agent_route_ws");
    auto project = ws_root / ".locus" / "prompts";
    auto global  = root    / "global";
    fs::create_directories(global);
    write_text(project / "ship_check.md",
               "Please review PR {0} for security gaps.\n");

    locus::Workspace ws(ws_root);

    locus::ToolRegistry tools;
    locus::register_builtin_tools(tools);

    StubLlm llm;

    locus::LLMConfig cfg;
    cfg.context_limit = 8192;

    locus::WorkspaceMetadata meta;
    meta.root = ws_root;

    locus::AgentCore agent(llm, tools, ws, "", meta, cfg,
                           ws_root / ".locus" / "sessions",
                           /*checkpoints_dir=*/{},
                           project, global);
    agent.start();

    agent.send_message_sync("/ship_check 1234");
    agent.stop();

    REQUIRE_THAT(llm.last_user_message,
                 ContainsSubstring("Please review PR 1234 for security gaps"));

    std::error_code rm_ec;
    fs::remove_all(root,    rm_ec);
    fs::remove_all(ws_root, rm_ec);
}

// -- AgentCore: unknown /foo with no template still surfaces unknown-cmd --

TEST_CASE("AgentCore: unknown slash with no matching template emits unknown-command",
          "[s4.x][prompt-templates][agent]")
{
    auto ws_root = make_temp_dir("unknown_slash_ws");
    auto project = ws_root / ".locus" / "prompts";
    fs::create_directories(project);

    locus::Workspace ws(ws_root);

    locus::ToolRegistry tools;
    locus::register_builtin_tools(tools);

    StubLlm llm;
    locus::LLMConfig cfg;
    cfg.context_limit = 8192;
    locus::WorkspaceMetadata meta;
    meta.root = ws_root;

    locus::AgentCore agent(llm, tools, ws, "", meta, cfg,
                           ws_root / ".locus" / "sessions",
                           /*checkpoints_dir=*/{},
                           project, fs::path{});

    CaptureFrontend fe;
    agent.register_frontend(&fe);
    agent.start();

    agent.send_message_sync("/this_is_not_a_command");
    agent.stop();
    agent.unregister_frontend(&fe);

    REQUIRE_THAT(fe.errors, ContainsSubstring("Unknown command"));
    // The stub LLM should NOT have been called because the slash was handled.
    REQUIRE(llm.last_user_message.empty());

    std::error_code rm_ec;
    fs::remove_all(ws_root, rm_ec);
}
