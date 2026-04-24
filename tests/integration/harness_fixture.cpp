#include "harness_fixture.h"

#include "core/watcher_pump.h"
#include "embedding_worker.h"
#include "indexer.h"
#include "tools/tools.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace locus::integration {

namespace fs = std::filesystem;

// -- PromptResult -------------------------------------------------------------

bool PromptResult::tool_called(std::string_view name) const
{
    return std::any_of(tool_calls.begin(), tool_calls.end(),
        [name](const ObservedToolCall& c) { return c.tool_name == name; });
}

const ObservedToolCall* PromptResult::find_tool_call(std::string_view name) const
{
    for (const auto& c : tool_calls)
        if (c.tool_name == name) return &c;
    return nullptr;
}

const ObservedToolResult* PromptResult::find_result_for(std::string_view tool_name) const
{
    // Find first tool_call matching name, then its result by id.
    const ObservedToolCall* call = find_tool_call(tool_name);
    if (!call) return nullptr;
    for (const auto& r : tool_results)
        if (r.call_id == call->id) return &r;
    return nullptr;
}

// -- Helpers ------------------------------------------------------------------

namespace {

std::string env_or(const char* name, const std::string& fallback)
{
    const char* v = std::getenv(name);
    if (v == nullptr || std::strlen(v) == 0) return fallback;
    return v;
}

fs::path resolve_workspace_root()
{
    // CMake injects LOCUS_INT_TEST_WORKSPACE_DIR as the repo root. Env var
    // override lets a user point at a different workspace if needed.
    std::string from_env = env_or("LOCUS_INT_TEST_WORKSPACE", "");
    if (!from_env.empty()) {
        fs::path p = fs::absolute(from_env);
        if (!fs::is_directory(p))
            throw std::runtime_error("LOCUS_INT_TEST_WORKSPACE is not a directory: " + p.string());
        return p;
    }

#ifdef LOCUS_INT_TEST_WORKSPACE_DIR
    fs::path p = fs::absolute(LOCUS_INT_TEST_WORKSPACE_DIR);
    if (!fs::is_directory(p))
        throw std::runtime_error("Compiled-in workspace path is not a directory: " + p.string());
    return p;
#else
    throw std::runtime_error(
        "No workspace path available. Set LOCUS_INT_TEST_WORKSPACE or "
        "build via CMake which defines LOCUS_INT_TEST_WORKSPACE_DIR.");
#endif
}

void init_integration_logging(const fs::path& locus_dir)
{
    static std::once_flag flag;
    std::call_once(flag, [&] {
        try {
            auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (locus_dir / "integration_test.log").string(),
                5 * 1024 * 1024, 2);

            stderr_sink->set_level(spdlog::level::warn);
            file_sink->set_level(spdlog::level::trace);

            auto logger = std::make_shared<spdlog::logger>(
                "locus", spdlog::sinks_init_list{stderr_sink, file_sink});
            logger->set_level(spdlog::level::trace);
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [t:%t] [%^%l%$] %v");
            spdlog::set_default_logger(logger);
            spdlog::flush_on(spdlog::level::warn);
        } catch (const spdlog::spdlog_ex& ex) {
            // Logger already configured by another consumer — ignore.
            (void)ex;
        }
    });
}

std::unique_ptr<IntegrationHarness>& instance_slot()
{
    static std::unique_ptr<IntegrationHarness> slot;
    return slot;
}

} // namespace

// -- IntegrationHarness -------------------------------------------------------

IntegrationHarness& IntegrationHarness::shared()
{
    auto& slot = instance_slot();
    if (!slot) {
        // Private ctor — use `new` then wrap.
        slot.reset(new IntegrationHarness());
    }
    return *slot;
}

void IntegrationHarness::shutdown_shared()
{
    instance_slot().reset();
}

IntegrationHarness::IntegrationHarness()
{
    workspace_root_ = resolve_workspace_root();
    tmp_dir_        = workspace_root_ / "tests" / "integration_tmp";

    // Fresh scratch dir. Nuke any leftover from a previous aborted run.
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
    fs::create_directories(tmp_dir_);

    init_integration_logging(workspace_root_ / ".locus");

    // LLM config from env (with sane defaults).
    llm_config_.base_url      = env_or("LOCUS_INT_TEST_ENDPOINT", "http://127.0.0.1:1234");
    llm_config_.model         = env_or("LOCUS_INT_TEST_MODEL", "");
    llm_config_.temperature   = 0.2;     // deterministic-ish
    llm_config_.max_tokens    = 2048;
    llm_config_.context_limit = 0;       // auto-detect
    llm_config_.timeout_ms    = 120000;  // local models can stall longer than usual

    llm_ = create_llm_client(llm_config_);

    // Reachability gate — THE requirement: fail fast if LM Studio is not up.
    ModelInfo mi = llm_->query_model_info();
    if (mi.id.empty()) {
        throw std::runtime_error(
            "LM Studio not reachable at " + llm_config_.base_url +
            " — start LM Studio and load a tool-calling model, or set "
            "LOCUS_INT_TEST_ENDPOINT to the correct URL.");
    }
    model_id_ = mi.id;
    if (llm_config_.model.empty()) llm_config_.model = mi.id;
    llm_config_.context_limit =
        mi.context_length > 0 ? mi.context_length : 8192;

    spdlog::info("Integration harness: LLM ok — model='{}', ctx={}",
                 llm_config_.model, llm_config_.context_limit);

    // Workspace (blocking initial index).
    workspace_ = std::make_unique<Workspace>(workspace_root_);

    const auto& st = workspace_->indexer().stats();
    spdlog::info("Integration harness: index ready — {} files, {} symbols, {} headings",
                 st.files_total, st.symbols_total, st.headings_total);

    // Tool registry.
    tools_ = std::make_unique<ToolRegistry>();
    register_builtin_tools(*tools_);

    // Force every tool through the approval gate so the harness sees a
    // uniform on_tool_call_pending callback regardless of each tool's default
    // policy (read_file / search / list_directory / get_file_outline are
    // auto-approve in normal operation — we override here so the test can
    // observe every tool invocation). The harness's on_tool_call_pending
    // auto-approves synchronously, so there's no real approval round-trip.
    for (auto* tool : tools_->all()) {
        workspace_->config().tool_approval_policies[tool->name()] =
            ToolApprovalPolicy::ask;
    }

    // Agent core.
    WorkspaceMetadata ws_meta;
    ws_meta.root          = workspace_root_;
    ws_meta.file_count    = static_cast<int>(st.files_total);
    ws_meta.symbol_count  = static_cast<int>(st.symbols_total);
    ws_meta.heading_count = static_cast<int>(st.headings_total);

    fs::path sessions_dir = tmp_dir_ / "sessions";
    fs::create_directories(sessions_dir);

    agent_ = std::make_unique<AgentCore>(
        *llm_, *tools_, *workspace_,
        workspace_->locus_md(), ws_meta, llm_config_, sessions_dir);

    frontend_ = std::make_unique<HarnessFrontend>();
    frontend_->attach_core(agent_.get());
    agent_->register_frontend(frontend_.get());

    // Route indexer activity (same as main.cpp).
    workspace_->indexer().on_activity =
        [this](const std::string& s, const std::string& d) {
            agent_->emit_index_event(s, d);
        };

    agent_->start();

    // Wait for any initial embedding backlog to drain so semantic queries are
    // meaningful on turn one.
    wait_for_embedding_idle();
}

IntegrationHarness::~IntegrationHarness()
{
    if (agent_) {
        agent_->stop();
        agent_->unregister_frontend(frontend_.get());
    }
    // Nuke scratch dir last (workspace destructors may flush events).
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
}

PromptResult IntegrationHarness::prompt(const std::string& text,
                                        std::chrono::milliseconds timeout)
{
    frontend_->reset_turn_state();

    // Mirrors main.cpp: flush file-watcher events before each turn so the LLM
    // sees a fresh index.
    workspace_->watcher_pump().flush_now();

    agent_->send_message(text);

    PromptResult r;
    r.timed_out    = !frontend_->wait_for_turn(timeout);
    r.tokens       = frontend_->tokens();
    r.tool_calls   = frontend_->tool_calls();
    r.tool_results = frontend_->tool_results();
    r.errors       = frontend_->errors();
    return r;
}

void IntegrationHarness::wait_for_index_idle(std::chrono::milliseconds timeout)
{
    workspace_->watcher_pump().flush_now();
    // After flush_now returns, the synchronous path has handed pending events
    // to the indexer. The indexer runs on the pump thread, so by the time
    // flush_now returns the batch has been fully processed. Keep a short tail
    // sleep in case more events were added concurrently by a prior tool call.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        workspace_->watcher_pump().flush_now();
        if (std::chrono::steady_clock::now() > deadline) break;
    }
}

void IntegrationHarness::wait_for_embedding_idle(std::chrono::milliseconds timeout)
{
    EmbeddingWorker* ew = workspace_->embedding_worker();
    if (!ew) return;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int stable_ticks = 0;
    int last_done = -1, last_total = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = ew->stats();
        if (s.done == s.total && s.done == last_done && s.total == last_total) {
            if (++stable_ticks >= 3) return;  // idle for ~300ms
        } else {
            stable_ticks = 0;
        }
        last_done = s.done;
        last_total = s.total;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    spdlog::warn("wait_for_embedding_idle: timed out");
}

} // namespace locus::integration
