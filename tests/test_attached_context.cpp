#include <catch2/catch_test_macros.hpp>

#include "agent/agent_core.h"
#include "core/frontend.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "support/fake_workspace_services.h"

#include <filesystem>
#include <optional>

using namespace locus;

namespace {

// Minimal ILLMClient that never streams -- AgentCore is exercised here only
// for its attached-context state and system-prompt composition; we never
// actually start the agent thread, so no LLM call is made.
class NullLLM : public ILLMClient {
public:
    void stream_completion(const std::vector<ChatMessage>&,
                           const std::vector<ToolSchema>&,
                           const StreamCallbacks&) override {}
    ModelInfo query_model_info() override { return {}; }
};

// Captures on_attached_context_changed broadcasts so the round-trip test
// can verify both the Core API result and the frontend fan-out.
class CapturingFrontend : public IFrontend {
public:
    void on_turn_start() override {}
    void on_token(std::string_view) override {}
    void on_tool_call_pending(const ToolCall&, const std::string&, bool,
                              const std::vector<std::string>&) override {}
    void on_tool_result(const std::string&, const std::string&, bool) override {}
    void on_turn_complete() override {}
    void on_context_meter(int, int, int, int) override {}
    void on_compaction_needed(int, int) override {}
    void on_session_reset() override {}
    void on_error(const std::string&) override {}
    void on_embedding_progress(int, int) override {}
    void on_activity(const ActivityEvent&) override {}
    void on_attached_context_changed(
        const std::optional<AttachedContext>& ctx) override
    {
        last = ctx;
        ++calls;
    }
    std::optional<AttachedContext> last;
    int calls = 0;
};

AgentCore make_core()
{
    static NullLLM      llm;
    static ToolRegistry reg;
    static bool         registered = false;
    if (!registered) {
        register_builtin_tools(reg);
        registered = true;
    }
    // index/embedder/workspace deliberately null -- outline lookup is skipped.
    static test::FakeWorkspaceServices services{std::filesystem::temp_directory_path()};
    WorkspaceMetadata meta;
    meta.root = services.root();
    LLMConfig cfg;
    return AgentCore(llm, reg, services, /*locus_md=*/"", meta, cfg,
                     std::filesystem::temp_directory_path());
}

} // namespace

TEST_CASE("Attached context: round-trip through Core", "[s2.4]")
{
    auto core = make_core();

    REQUIRE_FALSE(core.attached_context().has_value());

    AttachedContext ctx{"src/main.cpp", "entry point"};
    core.set_attached_context(ctx);

    auto got = core.attached_context();
    REQUIRE(got.has_value());
    CHECK(got->file_path == "src/main.cpp");
    CHECK(got->preview   == "entry point");

    core.clear_attached_context();
    REQUIRE_FALSE(core.attached_context().has_value());
}

TEST_CASE("Attached context: frontends are notified", "[s2.4]")
{
    auto core = make_core();
    CapturingFrontend fe;
    core.register_frontend(&fe);

    core.set_attached_context({"docs/README.md", ""});
    REQUIRE(fe.calls == 1);
    REQUIRE(fe.last.has_value());
    CHECK(fe.last->file_path == "docs/README.md");

    core.clear_attached_context();
    CHECK(fe.calls == 2);
    CHECK_FALSE(fe.last.has_value());

    // clear when already empty must not refire (would spam the chip widget).
    core.clear_attached_context();
    CHECK(fe.calls == 2);

    core.unregister_frontend(&fe);
}

TEST_CASE("S4.F: attach/detach keeps system prompt byte-stable", "[s2.4][s4.f]")
{
    auto core = make_core();

    // System message must NOT mention the attached file -- the block now
    // rides on the next user message instead (S4.F: keep the prefix cache
    // alive).
    const std::string sys_before = core.history().messages().front().content;
    REQUIRE(sys_before.find("[Attached:")      == std::string::npos);
    REQUIRE(sys_before.find("## Attached File") == std::string::npos);

    core.set_attached_context({"include/widget.h", "header for the widget"});

    const std::string sys_attached = core.history().messages().front().content;
    REQUIRE(sys_attached == sys_before);  // byte-stable -- KV cache stays alive

    // The block is exposed for tests + future frontend uses.
    std::string block = core.attached_context_block();
    REQUIRE_FALSE(block.empty());
    REQUIRE(block.find("[Attached file: include/widget.h]") != std::string::npos);
    REQUIRE(block.find("Preview: header for the widget")    != std::string::npos);

    core.clear_attached_context();

    const std::string sys_cleared = core.history().messages().front().content;
    REQUIRE(sys_cleared == sys_before);  // still byte-stable after detach
    REQUIRE(core.attached_context_block().empty());
}

TEST_CASE("S4.F: attached pin survives reset_conversation; prompt stays stable",
          "[s2.4][s4.f]")
{
    auto core = make_core();
    const std::string sys_before = core.history().messages().front().content;

    core.set_attached_context({"path/to/file.txt", ""});
    core.reset_conversation();

    // Pin must persist -- the user's attachment isn't owned by a conversation.
    REQUIRE(core.attached_context().has_value());
    // System prompt must still be byte-stable, both vs. before-attach and
    // vs. after the reset.
    REQUIRE(core.history().messages().front().content == sys_before);
    // The block follows the pin -- next user turn picks it up.
    REQUIRE(core.attached_context_block().find("path/to/file.txt")
            != std::string::npos);
}
