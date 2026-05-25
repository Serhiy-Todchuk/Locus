// S5.D -- Reserve Enforcement + Token-Cost X-Ray unit tests.
//
// Covers:
//   reserve math: ContextBudget set_reserve / effective_limit / check_overflow
//   LLMContext::would_breach_reserve
//   per-message token_estimate: populated on add(), skipped when pre-set
//   from_json back-fill for pre-S5.D session files
//   estimate_message: framing + content + tool_calls components
//   WorkspaceConfig: compaction_reserve_tokens / ui_show_per_message_tokens
//     JSON round-trip and defaults

#include <catch2/catch_test_macros.hpp>

#include "agent/context_budget.h"
#include "agent/conversation.h"
#include "agent/llm_context.h"
#include "agent/session_manager.h"
#include "agent/system_prompt.h"
#include "agent/system_prompt_assembly.h"
#include "core/frontend_registry.h"
#include "core/workspace.h"
#include "core/workspace_config_json.h"
#include "llm/llm_client.h"
#include "llm/token_counter.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "support/fake_workspace_services.h"

#include <filesystem>
#include <random>

using namespace locus;
namespace fs = std::filesystem;

namespace {

fs::path make_tmp_dir(const std::string& tag)
{
    std::random_device rd;
    auto p = fs::temp_directory_path()
             / ("locus_s5d_" + tag + "_" + std::to_string(rd()));
    fs::create_directories(p);
    return p;
}

struct Fixture {
    ToolRegistry                tools;
    test::FakeWorkspaceServices services;
    FrontendRegistry            frontends;
    SessionManager              sessions;
    fs::path                    checkpoints_dir;

    explicit Fixture(const std::string& tag)
        : services(make_tmp_dir(tag + "_ws"))
        , sessions(make_tmp_dir(tag + "_sess"))
        , checkpoints_dir(make_tmp_dir(tag + "_ckpt"))
    {
        register_builtin_tools(tools);
    }

    std::unique_ptr<LLMContext> make_context(LLMConfig cfg = default_cfg())
    {
        WorkspaceMetadata meta;
        meta.root = services.root();
        auto prompt = SystemPromptAssembly::build("", meta, tools, cfg.tool_format);
        return std::make_unique<LLMContext>(
            services, cfg, std::move(prompt),
            frontends, sessions, checkpoints_dir);
    }

    static LLMConfig default_cfg()
    {
        LLMConfig c;
        c.context_limit = 8192;
        return c;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// ContextBudget reserve math
// ---------------------------------------------------------------------------

TEST_CASE("[s5.d][reserve] ContextBudget initial state has no reserve",
          "[s5.d][reserve]")
{
    FrontendRegistry fr;
    ContextBudget b(8192, fr);
    REQUIRE(b.reserve() == 0);
    REQUIRE(b.effective_limit() == 8192);
}

TEST_CASE("[s5.d][reserve] ContextBudget set_reserve and effective_limit",
          "[s5.d][reserve]")
{
    FrontendRegistry fr;
    ContextBudget b(8192, fr);

    b.set_reserve(1024);
    REQUIRE(b.reserve() == 1024);
    REQUIRE(b.effective_limit() == 7168);  // 8192 - 1024

    b.set_reserve(0);
    REQUIRE(b.reserve() == 0);
    REQUIRE(b.effective_limit() == 8192);
}

TEST_CASE("[s5.d][reserve] ContextBudget negative reserve is clamped to 0",
          "[s5.d][reserve]")
{
    FrontendRegistry fr;
    ContextBudget b(8192, fr);
    b.set_reserve(-500);
    REQUIRE(b.reserve() == 0);
}

TEST_CASE("[s5.d][reserve] ContextBudget effective_limit never below 1",
          "[s5.d][reserve]")
{
    FrontendRegistry fr;
    ContextBudget b(0, fr);  // limit=0 (not yet probed)
    b.set_reserve(512);
    REQUIRE(b.effective_limit() >= 1);
}

TEST_CASE("[s5.d][reserve] ContextBudget large reserve clamped at effective_limit",
          "[s5.d][reserve]")
{
    FrontendRegistry fr;
    ContextBudget b(1000, fr);
    b.set_reserve(2000);  // reserve > limit
    // effective_limit = std::max(1, 1000-2000) = 1
    REQUIRE(b.effective_limit() >= 1);
}

// ---------------------------------------------------------------------------
// LLMContext::would_breach_reserve
// ---------------------------------------------------------------------------

TEST_CASE("[s5.d][reserve] would_breach_reserve false when no reserve set",
          "[s5.d][reserve]")
{
    Fixture f("breach_none");
    auto ctx = f.make_context();
    // FakeWorkspaceServices has no real workspace(), so update_reserve() is
    // a no-op and reserve stays 0. Any prompt size is fine.
    REQUIRE_FALSE(ctx->would_breach_reserve(8192));
    REQUIRE_FALSE(ctx->would_breach_reserve(100000));
}

TEST_CASE("[s5.d][reserve] would_breach_reserve transitions at limit-reserve",
          "[s5.d][reserve]")
{
    Fixture f("breach_transitions");
    auto ctx = f.make_context();
    ctx->budget().set_reserve(1000);

    // 8192 - 1000 = 7192.
    REQUIRE_FALSE(ctx->would_breach_reserve(7191));
    REQUIRE_FALSE(ctx->would_breach_reserve(7192));  // exactly at threshold: ok
    REQUIRE(ctx->would_breach_reserve(7193));         // one over: breach
}

TEST_CASE("[s5.d][reserve] effective_limit via LLMContext reflects budget reserve",
          "[s5.d][reserve]")
{
    Fixture f("eff_limit");
    auto ctx = f.make_context();
    ctx->budget().set_reserve(400);
    REQUIRE(ctx->effective_limit() == 8192 - 400);
    REQUIRE(ctx->reserve_tokens() == 400);
}

// ---------------------------------------------------------------------------
// Per-message token_estimate
// ---------------------------------------------------------------------------

TEST_CASE("[s5.d][token-estimate] estimate_message includes framing overhead",
          "[s5.d][token-estimate]")
{
    ChatMessage m;
    m.role    = MessageRole::user;
    m.content = "Hello!";

    int est = TokenCounter::estimate_message(m);
    // Must be > content-only estimate due to the 4-token framing.
    REQUIRE(est > TokenCounter::estimate(m.content));
    REQUIRE(est >= 4);
}

TEST_CASE("[s5.d][token-estimate] estimate_message adds tool_call tokens",
          "[s5.d][token-estimate]")
{
    ChatMessage base_m;
    base_m.role    = MessageRole::assistant;
    base_m.content = "";

    ChatMessage with_tool = base_m;
    ToolCallRequest tc;
    tc.name      = "read_file";
    tc.arguments = "{\"path\":\"src/main.cpp\"}";
    with_tool.tool_calls.push_back(tc);

    REQUIRE(TokenCounter::estimate_message(with_tool) >
            TokenCounter::estimate_message(base_m));
}

TEST_CASE("[s5.d][token-estimate] ConversationHistory::add populates token_estimate",
          "[s5.d][token-estimate]")
{
    ConversationHistory h;
    ChatMessage m;
    m.role    = MessageRole::user;
    m.content = std::string(400, 'x');  // ~100 tokens content + 4 framing

    h.add(m);

    REQUIRE(h.messages().size() == 1);
    int est = h.messages().back().token_estimate;
    REQUIRE(est > 0);
    REQUIRE(est >= 100);  // 400 chars / 4 chars-per-token
}

TEST_CASE("[s5.d][token-estimate] add() does not overwrite pre-set token_estimate",
          "[s5.d][token-estimate]")
{
    ConversationHistory h;
    ChatMessage m;
    m.role           = MessageRole::user;
    m.content        = "short";
    m.token_estimate = 9999;  // sentinel

    h.add(m);
    REQUIRE(h.messages().back().token_estimate == 9999);
}

TEST_CASE("[s5.d][token-estimate] all messages have positive token_estimate after add",
          "[s5.d][token-estimate]")
{
    ConversationHistory h;
    for (int i = 0; i < 5; ++i) {
        ChatMessage m;
        m.role    = (i % 2 == 0) ? MessageRole::user : MessageRole::assistant;
        m.content = "message " + std::to_string(i);
        h.add(m);
    }

    for (const auto& msg : h.messages())
        REQUIRE(msg.token_estimate > 0);
}

TEST_CASE("[s5.d][token-estimate] from_json back-fills estimate for legacy messages",
          "[s5.d][token-estimate]")
{
    nlohmann::json j = nlohmann::json::array();
    j.push_back({{"role","user"},{"content","a message without token estimate"}});
    j.push_back({{"role","assistant"},{"content","a reply"}});

    auto h = ConversationHistory::from_json(j);
    for (const auto& msg : h.messages())
        REQUIRE(msg.token_estimate > 0);
}

// ---------------------------------------------------------------------------
// WorkspaceConfig JSON round-trip
// ---------------------------------------------------------------------------

TEST_CASE("[s5.d][config] compaction_reserve_tokens default is -1 (auto)",
          "[s5.d][config]")
{
    WorkspaceConfig cfg;
    REQUIRE(cfg.compaction.reserve_tokens == -1);
}

TEST_CASE("[s5.d][config] compaction_reserve_tokens round-trips through JSON",
          "[s5.d][config]")
{
    WorkspaceConfig cfg;
    cfg.compaction.reserve_tokens = 2048;

    auto j    = workspace_config_to_json(cfg);
    REQUIRE(j.contains("compaction"));
    REQUIRE(j["compaction"].contains("reserve_tokens"));
    REQUIRE(j["compaction"]["reserve_tokens"] == 2048);

    auto cfg2 = workspace_config_from_json(j);
    REQUIRE(cfg2.compaction.reserve_tokens == 2048);
}

TEST_CASE("[s5.d][config] ui_show_per_message_tokens default is true",
          "[s5.d][config]")
{
    WorkspaceConfig cfg;
    REQUIRE(cfg.chat.show_per_message_tokens == true);
}

TEST_CASE("[s5.d][config] ui_show_per_message_tokens round-trips through JSON",
          "[s5.d][config]")
{
    WorkspaceConfig cfg;
    cfg.chat.show_per_message_tokens = false;

    auto j = workspace_config_to_json(cfg);
    REQUIRE(j.contains("ui"));
    REQUIRE(j["ui"].contains("show_per_message_tokens"));
    REQUIRE(j["ui"]["show_per_message_tokens"] == false);

    auto cfg2 = workspace_config_from_json(j);
    REQUIRE(cfg2.chat.show_per_message_tokens == false);
}
