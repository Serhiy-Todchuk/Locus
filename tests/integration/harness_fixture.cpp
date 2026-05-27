#include "harness_fixture.h"

#include "core/watcher_pump.h"
#include "index/embedding_worker.h"
#include "index/indexer.h"
#include "tools/tools.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#if defined(_WIN32) && defined(_DEBUG)
#include <spdlog/sinks/msvc_sink.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
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

std::atomic<bool> g_console_logging{false};

void init_integration_logging(const fs::path& locus_dir)
{
    static std::once_flag flag;
    std::call_once(flag, [&] {
        try {
            auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (locus_dir / "integration_test.log").string(),
                5 * 1024 * 1024, 2);

            stderr_sink->set_level(g_console_logging.load()
                                       ? spdlog::level::trace
                                       : spdlog::level::warn);
            file_sink->set_level(spdlog::level::trace);

            std::vector<spdlog::sink_ptr> sinks{ stderr_sink, file_sink };
#if defined(_WIN32) && defined(_DEBUG)
            auto msvc_sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
            msvc_sink->set_level(spdlog::level::trace);
            sinks.push_back(msvc_sink);
#endif

            auto logger = std::make_shared<spdlog::logger>(
                "locus", sinks.begin(), sinks.end());
            logger->set_level(spdlog::level::trace);
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [t:%t] [%^%l%$] %v");
            spdlog::set_default_logger(logger);
            spdlog::flush_on(spdlog::level::warn);
        } catch (const spdlog::spdlog_ex& ex) {
            // Logger already configured by another consumer -- ignore.
            (void)ex;
        }
    });
}

std::unique_ptr<IntegrationHarness>& instance_slot()
{
    static std::unique_ptr<IntegrationHarness> slot;
    return slot;
}

// Probe LM Studio's proprietary /api/v0/models/<id> endpoint for the
// loaded model's `capabilities[]`. Returns:
//   - vector with one or more entries  -> server advertised capabilities
//   - empty vector                     -> endpoint returned 200 but no caps
//   - std::nullopt                     -> endpoint missing / not JSON / unreachable
// Non-LM-Studio backends (Ollama, llama-server) don't implement /api/v0/, so
// `std::nullopt` is the "unknown -- can't enforce" signal. Reachability is
// already proven separately via `query_model_info()` -- we only call this
// after that has succeeded.
std::optional<std::vector<std::string>>
fetch_model_capabilities(const std::string& base_url, const std::string& model_id)
{
    if (base_url.empty() || model_id.empty()) return std::nullopt;
    std::string url = base_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/api/v0/models/" + model_id;

    cpr::Response r = cpr::Get(cpr::Url{url},
                               cpr::Timeout{std::chrono::milliseconds(3000)});
    if (r.status_code != 200) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(r.text);
        if (!j.contains("capabilities") || !j["capabilities"].is_array())
            return std::vector<std::string>{};
        std::vector<std::string> out;
        for (const auto& c : j["capabilities"])
            if (c.is_string()) out.push_back(c.get<std::string>());
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

// -- Console logging toggle ---------------------------------------------------

void set_console_logging(bool enabled) { g_console_logging.store(enabled); }
bool is_console_logging()              { return g_console_logging.load(); }

// -- IntegrationHarness -------------------------------------------------------

IntegrationHarness& IntegrationHarness::shared()
{
    auto& slot = instance_slot();
    if (!slot) {
        // Private ctor -- use `new` then wrap.
        slot.reset(new IntegrationHarness());
    }
    return *slot;
}

IntegrationHarness* IntegrationHarness::shared_if_alive()
{
    return instance_slot().get();
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

    // Reachability gate -- THE requirement: fail fast if LM Studio is not up.
    ModelInfo mi = llm_->query_model_info();
    if (mi.id.empty()) {
        throw std::runtime_error(
            "LM Studio not reachable at " + llm_config_.base_url +
            " -- start LM Studio and load a tool-calling model, or set "
            "LOCUS_INT_TEST_ENDPOINT to the correct URL.");
    }
    model_id_ = mi.id;
    if (llm_config_.model.empty()) llm_config_.model = mi.id;
    llm_config_.context_limit =
        mi.context_length > 0 ? mi.context_length : 8192;

    spdlog::info("Integration harness: LLM ok -- model='{}', ctx={}",
                 llm_config_.model, llm_config_.context_limit);

    // Capability gate: if the server advertises capabilities (LM Studio's
    // proprietary /api/v0/models endpoint does), require `tool_use`. The
    // whole integration suite is tool-driven; a model that rejects a
    // `tools=[]` payload fails fast with HTTP 400 deep inside the agent loop
    // and produces a confusing test failure. This trips an explicit error at
    // harness construction instead. Non-LM-Studio backends don't implement
    // the v0 endpoint -- we treat that as "unknown" and proceed.
    auto caps = fetch_model_capabilities(llm_config_.base_url, model_id_);
    if (caps.has_value()) {
        bool has_tool_use = std::any_of(
            caps->begin(), caps->end(),
            [](const std::string& s) { return s == "tool_use"; });
        if (!has_tool_use) {
            std::string advertised;
            for (size_t i = 0; i < caps->size(); ++i) {
                if (i) advertised += ", ";
                advertised += (*caps)[i];
            }
            if (advertised.empty()) advertised = "(none)";
            throw std::runtime_error(
                "Loaded model '" + model_id_ + "' does not advertise the "
                "'tool_use' capability (advertised: " + advertised + "). The "
                "integration suite is tool-driven and will fail. Load a "
                "tool-calling model in LM Studio (Gemma 4 E4B / Qwen 3 14B / "
                "Qwen 3.6 / etc.) or pin a specific id via LOCUS_INT_TEST_MODEL.");
        }
        spdlog::info("Integration harness: model capabilities verified "
                     "(tool_use present)");
    } else {
        spdlog::info("Integration harness: model capabilities unknown "
                     "(non-LM-Studio backend or v0 API unavailable) -- "
                     "proceeding without capability enforcement");
    }

    // Workspace (blocking initial index).
    workspace_ = std::make_unique<Workspace>(workspace_root_);

    // The repo's .gitignore excludes tests/integration_tmp/ -- the harness
    // writes its scratch files there and the indexer needs to see them.
    // Turn gitignore respect off for the test session and reload so the
    // already-merged patterns get cleared. This only affects the live
    // process; .locus/config.json on disk is untouched (we never save it
    // from the harness).
    if (workspace_->config().index.respect_gitignore) {
        workspace_->config().index.respect_gitignore = false;
        workspace_->indexer().reload_gitignore();
        spdlog::info("Integration harness: disabled .gitignore respect for "
                     "scratch-dir visibility (tests/integration_tmp).");
    }

    const auto& st = workspace_->indexer().stats();
    spdlog::info("Integration harness: index ready -- {} files, {} symbols, {} headings",
                 st.files_total, st.symbols_total, st.headings_total);

    // Tool registry.
    tools_ = std::make_unique<ToolRegistry>();
    register_builtin_tools(*tools_);

    // Route every mutating tool through the approval gate so the harness can
    // exercise the gate path (the harness frontend auto-approves synchronously
    // inside on_tool_call_pending, so there's no real round-trip; the wait
    // path is exercised in test_tool_safety). Read-category tools are
    // hard-forced to auto_approve by the dispatcher regardless of override
    // (the workspace boundary is the trust boundary -- you opened the
    // folder); their entries here are silently ignored at dispatch time but
    // do no harm. The harness's on_tool_call_pending callback fires for
    // every dispatch regardless of policy, so `tool_called(name)` /
    // `find_result_for(name)` introspection works for read tools too.
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

    fs::path sessions_dir    = tmp_dir_ / "sessions";
    fs::path checkpoints_dir = tmp_dir_ / "checkpoints";
    fs::path prompts_dir     = tmp_dir_ / "prompts";   // S4.X -- project templates
    fs::create_directories(sessions_dir);
    fs::create_directories(checkpoints_dir);
    fs::create_directories(prompts_dir);

    // No global prompts dir in tests -- pollutes the user's APPDATA otherwise.
    agent_ = std::make_unique<AgentCore>(
        *llm_, *tools_, *workspace_,
        workspace_->locus_md(), ws_meta, llm_config_,
        sessions_dir, checkpoints_dir,
        prompts_dir, /*global_prompts_dir=*/fs::path{});

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
    // meaningful on turn one. The harness ctor can't REQUIRE (no Catch test
    // context yet), so we just log; per-test wait_for_embedding_idle calls
    // are the enforcement point.
    if (!wait_for_embedding_idle()) {
        spdlog::error("Initial embedding did not finish within timeout - "
                      "semantic test cases will likely fail");
    }
}

IntegrationHarness::~IntegrationHarness()
{
    // Drop callbacks that capture `agent_` BEFORE tabs are gone. The
    // workspace dtor calls `watcher_pump_->stop()` which performs one last
    // synchronous flush; an in-flight file event there fires the indexer's
    // on_activity callback. If we let `agent_` destruct first (reverse-
    // declaration order: agent_ before workspace_), the captured raw pointer
    // dangles. Mirrors `LocusSession::~LocusSession` -- same hazard.
    if (workspace_) {
        workspace_->indexer().on_activity = nullptr;
        if (auto* ew = workspace_->embedding_worker())
            ew->on_activity = nullptr;
    }
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
    // efsw on Windows can take a couple of hundred milliseconds to surface a
    // file-change event after the syscall returns. We pump aggressively so a
    // freshly-written file is indexed before we hand control back to the
    // caller. flush_now() bypasses the watcher's 200 ms debounce, so each
    // iteration consumes whatever has arrived since the previous one without
    // waiting for the coalesce window.
    //
    // Min ~1.2 s covers the slow-efsw cases observed on Windows under AV
    // load; capped at `timeout` so a buggy indexer can't stall the suite.
    const auto deadline   = std::chrono::steady_clock::now() + timeout;
    const auto min_total  = std::chrono::milliseconds(1200);
    const auto floor_end  = std::chrono::steady_clock::now() + min_total;
    workspace_->watcher_pump().flush_now();
    for (int i = 0; i < 24; ++i) {
        auto now = std::chrono::steady_clock::now();
        if (now > deadline) break;
        if (i >= 8 && now > floor_end) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        workspace_->watcher_pump().flush_now();
    }
}

bool IntegrationHarness::wait_for_embedding_idle(std::chrono::milliseconds timeout)
{
    EmbeddingWorker* ew = workspace_->embedding_worker();
    if (!ew) return true;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int stable_ticks = 0;
    int last_done = -1, last_total = -1;
    auto last_log = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = ew->stats();
        if (s.done == s.total && s.done == last_done && s.total == last_total) {
            if (++stable_ticks >= 3) return true;  // idle for ~300ms
        } else {
            stable_ticks = 0;
        }
        // Heartbeat every 30s so a long cold-embed run shows progress in
        // the console rather than looking hung.
        auto now = std::chrono::steady_clock::now();
        if (now - last_log > std::chrono::seconds(30)) {
            spdlog::info("wait_for_embedding_idle: {}/{} chunks embedded",
                         s.done, s.total);
            last_log = now;
        }
        last_done = s.done;
        last_total = s.total;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    auto s = ew->stats();
    spdlog::warn("wait_for_embedding_idle: timed out at {}/{} after {} ms",
                 s.done, s.total, timeout.count());
    return false;
}

} // namespace locus::integration
