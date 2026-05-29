#include <catch2/catch_test_macros.hpp>

#include "agent/agent_core.h"
#include "core/frontend.h"
#include "llm/endpoint_profile.h"
#include "llm/openai_transport.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "support/fake_workspace_services.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace locus;
namespace fs = std::filesystem;

namespace {

void set_env(const char* name, const std::string& value)
{
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

fs::path unique_tmp_dir(const std::string& tag)
{
    auto base = fs::temp_directory_path() /
                ("locus_ep_" + tag + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(&tag)));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

} // namespace

// -- Task A: EndpointProfileStore ---------------------------------------------

TEST_CASE("EndpointProfileStore: seeds six builtins on missing file", "[s6.16]")
{
    auto dir = unique_tmp_dir("seed");
    auto path = dir / "endpoints.json";

    EndpointProfileStore store;
    bool existed = store.load(path);

    CHECK_FALSE(existed);                       // file was absent
    CHECK(store.list().size() == 6);
    CHECK(store.active() == "LM Studio (local)");

    // All six are builtins.
    for (const auto& p : store.list())
        CHECK(p.builtin);

    // NVIDIA seed carries the documented default model + tool_format.
    const auto* nv = store.find("NVIDIA Build (hosted)");
    REQUIRE(nv != nullptr);
    CHECK(nv->base_url == "https://integrate.api.nvidia.com/v1");
    CHECK(nv->default_model == "qwen/qwen3-coder-480b-a35b-instruct");
    CHECK(nv->tool_format == "openai");
}

TEST_CASE("EndpointProfileStore: save + reload round-trip", "[s6.16]")
{
    auto dir  = unique_tmp_dir("roundtrip");
    auto path = dir / "endpoints.json";

    EndpointProfileStore store;
    store.load(path);                            // seeds

    EndpointProfile custom;
    custom.name = "My NVIDIA";
    custom.base_url = "https://integrate.api.nvidia.com/v1";
    custom.api_key = "nvapi-secret-key";
    custom.default_model = "qwen/qwen3-coder-480b-a35b-instruct";
    custom.tool_format = "openai";
    custom.extra_headers = {{"X-Title", "Locus"}};
    REQUIRE(store.add(custom));
    REQUIRE(store.set_active("My NVIDIA"));
    REQUIRE(store.save(path).empty());           // empty = success

    EndpointProfileStore reloaded;
    REQUIRE(reloaded.load(path));                // file now exists + parses
    CHECK(reloaded.active() == "My NVIDIA");
    const auto* got = reloaded.find("My NVIDIA");
    REQUIRE(got != nullptr);
    CHECK(got->api_key == "nvapi-secret-key");
    CHECK(got->default_model == "qwen/qwen3-coder-480b-a35b-instruct");
    CHECK(got->tool_format == "openai");
    REQUIRE(got->extra_headers.count("X-Title") == 1);
    CHECK(got->extra_headers.at("X-Title") == "Locus");
    CHECK_FALSE(got->builtin);                    // user-added stays non-builtin
}

TEST_CASE("EndpointProfileStore: name uniqueness + builtin protection", "[s6.16]")
{
    EndpointProfileStore store;
    store.seed_defaults();

    // Duplicate name refused.
    EndpointProfile dup;
    dup.name = "LM Studio (local)";
    dup.base_url = "http://elsewhere";
    CHECK_FALSE(store.add(dup));

    // Empty name refused.
    EndpointProfile blank;
    CHECK_FALSE(store.add(blank));

    // Builtins cannot be removed.
    CHECK_FALSE(store.remove("LM Studio (local)"));
    CHECK(store.list().size() == 6);

    // Non-builtin can be added then removed.
    EndpointProfile mine;
    mine.name = "Mine";
    mine.base_url = "http://127.0.0.1:7777";
    REQUIRE(store.add(mine));
    CHECK(store.list().size() == 7);
    CHECK(store.remove("Mine"));
    CHECK(store.list().size() == 6);

    // set_active refuses unknown.
    CHECK_FALSE(store.set_active("does-not-exist"));
}

TEST_CASE("EndpointProfileStore: update preserves builtin flag", "[s6.16]")
{
    EndpointProfileStore store;
    store.seed_defaults();

    EndpointProfile edit;
    edit.name = "NVIDIA Build (hosted)";
    edit.base_url = "https://integrate.api.nvidia.com/v1";
    edit.api_key = "nvapi-edited";
    edit.builtin = false;                         // user tries to clear -- ignored
    REQUIRE(store.update(edit));

    const auto* got = store.find("NVIDIA Build (hosted)");
    REQUIRE(got != nullptr);
    CHECK(got->api_key == "nvapi-edited");
    CHECK(got->builtin);                          // still builtin
}

// -- Task B: transport header builder -----------------------------------------

TEST_CASE("build_request_headers: no auth when key empty", "[s6.16]")
{
    LLMConfig cfg;
    cfg.api_key = "";
    auto h = build_request_headers(cfg);

    bool has_ct = false, has_accept = false, has_auth = false;
    for (auto& [k, v] : h) {
        if (k == "Content-Type") has_ct = true;
        if (k == "Accept")       has_accept = true;
        if (k == "Authorization") has_auth = true;
    }
    CHECK(has_ct);
    CHECK(has_accept);
    CHECK_FALSE(has_auth);
}

TEST_CASE("build_request_headers: Bearer prefix + extra headers merged", "[s6.16]")
{
    LLMConfig cfg;
    cfg.api_key = "nvapi-xyz";
    cfg.extra_headers = {{"HTTP-Referer", "https://locus.local"},
                         {"X-Title", "Locus"}};
    auto h = build_request_headers(cfg);

    std::string auth, referer, title;
    for (auto& [k, v] : h) {
        if (k == "Authorization") auth = v;
        if (k == "HTTP-Referer")  referer = v;
        if (k == "X-Title")       title = v;
    }
    CHECK(auth == "Bearer nvapi-xyz");
    CHECK(referer == "https://locus.local");
    CHECK(title == "Locus");
}

// -- Task B: URL join (avoid /v1 doubling) ------------------------------------

TEST_CASE("join_api_url: no doubling when base ends in /v1", "[s6.16]")
{
    // LM Studio style: host-only base.
    CHECK(join_api_url("http://127.0.0.1:1234", "/v1/chat/completions")
          == "http://127.0.0.1:1234/v1/chat/completions");
    // Hosted style: base already carries /v1.
    CHECK(join_api_url("https://integrate.api.nvidia.com/v1", "/v1/chat/completions")
          == "https://integrate.api.nvidia.com/v1/chat/completions");
    CHECK(join_api_url("http://127.0.0.1:11434/v1", "/v1/models")
          == "http://127.0.0.1:11434/v1/models");
    // Trailing slash normalised.
    CHECK(join_api_url("https://api.openai.com/v1/", "/v1/chat/completions")
          == "https://api.openai.com/v1/chat/completions");
    // Non-/v1 path (LM Studio proprietary) is appended verbatim.
    CHECK(join_api_url("http://127.0.0.1:1234", "/api/v0/models")
          == "http://127.0.0.1:1234/api/v0/models");
}

// -- Task C: profile resolution layering --------------------------------------

TEST_CASE("apply_endpoint_profile: startup fills only unset sentinels", "[s6.16]")
{
    EndpointProfile p;
    p.base_url = "https://integrate.api.nvidia.com/v1";
    p.api_key = "nvapi-k";
    p.default_model = "qwen/qwen3-coder-480b-a35b-instruct";
    p.default_context_limit = 40000;
    p.tool_format = "openai";

    SECTION("default config adopts profile") {
        LLMConfig cfg;   // base_url at the hardcoded default
        apply_endpoint_profile(cfg, p, /*force=*/false);
        CHECK(cfg.base_url == "https://integrate.api.nvidia.com/v1");
        CHECK(cfg.api_key == "nvapi-k");
        CHECK(cfg.model == "qwen/qwen3-coder-480b-a35b-instruct");
        CHECK(cfg.context_limit == 40000);
        CHECK(cfg.tool_format == ToolFormat::OpenAi);
    }

    SECTION("non-default legacy endpoint wins over profile") {
        LLMConfig cfg;
        cfg.base_url = "http://192.168.1.50:1234";   // legacy / explicit
        cfg.model = "local-model";
        apply_endpoint_profile(cfg, p, /*force=*/false);
        CHECK(cfg.base_url == "http://192.168.1.50:1234");  // legacy wins
        CHECK(cfg.model == "local-model");                  // legacy wins
        CHECK(cfg.api_key == "nvapi-k");                    // no legacy equiv
    }
}

TEST_CASE("apply_endpoint_profile: force overlays unconditionally", "[s6.16]")
{
    EndpointProfile p;
    p.base_url = "https://api.openai.com/v1";
    p.api_key = "sk-abc";
    p.default_model = "";          // server default
    p.default_context_limit = 0;   // re-detect
    p.tool_format = "openai";

    LLMConfig cfg;
    cfg.base_url = "http://192.168.1.50:1234";
    cfg.model = "old-model";
    cfg.context_limit = 8000;
    cfg.api_key = "stale";

    apply_endpoint_profile(cfg, p, /*force=*/true);
    CHECK(cfg.base_url == "https://api.openai.com/v1");
    CHECK(cfg.api_key == "sk-abc");
    CHECK(cfg.model.empty());        // profile default wins (re-detect later)
    CHECK(cfg.context_limit == 0);   // re-detect later
    CHECK(cfg.tool_format == ToolFormat::OpenAi);
}

// -- Task D: AgentCore endpoint hot-swap --------------------------------------

namespace {

class NullLLM : public ILLMClient {
public:
    void stream_completion(const std::vector<ChatMessage>&,
                           const std::vector<ToolSchema>&,
                           const StreamCallbacks&) override {}
    ModelInfo query_model_info() override { return {}; }
};

class EndpointCapture : public IFrontend {
public:
    void on_turn_start() override {}
    void on_token(std::string_view) override {}
    void on_tool_call_pending(const ToolCall&, const std::string&, bool,
                              const std::vector<std::string>&) override {}
    void on_tool_result(const std::string&, const std::string&, bool) override {}
    void on_turn_complete() override {}
    void on_context_meter(int, int, int, int, int, long long) override {}
    void on_compaction_needed(int, int) override {}
    void on_session_reset() override {}
    void on_error(const std::string&) override {}
    void on_embedding_progress(int, int) override {}
    void on_activity(const ActivityEvent&) override {}
    void on_endpoint_changed(const std::string& name, const std::string& model,
                             int limit) override
    {
        last_name = name;
        last_model = model;
        last_limit = limit;
        ++calls;
    }
    std::string last_name, last_model;
    int last_limit = 0;
    int calls = 0;
};

bool wait_until(std::function<bool()> pred, int timeout_ms = 20000)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return pred();
}

} // namespace

TEST_CASE("AgentCore: endpoint hot-swap reseats client + broadcasts", "[s6.16]")
{
    auto dir = unique_tmp_dir("swap");
    set_env("LOCUS_GLOBAL_DIR", dir.string());

    // Seed a store with a custom profile pointing at an unreachable local
    // port -- query_model_info fails fast (connection refused), so the
    // profile's default_model / default_context_limit are the deterministic
    // post-swap values (no network dependency).
    {
        EndpointProfileStore store;
        store.load();   // seeds + uses LOCUS_GLOBAL_DIR
        EndpointProfile p;
        p.name = "Stub (unreachable)";
        p.base_url = "http://127.0.0.1:9";
        p.default_model = "stub-model";
        p.default_context_limit = 4242;
        p.tool_format = "openai";
        REQUIRE(store.add(p));
        REQUIRE(store.save().empty());
    }

    NullLLM llm;
    ToolRegistry reg;
    register_builtin_tools(reg);
    test::FakeWorkspaceServices services{fs::temp_directory_path()};
    WorkspaceMetadata meta;
    meta.root = services.root();
    LLMConfig cfg;

    AgentCore core(llm, reg, services, "", meta, cfg,
                   fs::temp_directory_path());
    EndpointCapture fe;
    core.register_frontend(&fe);
    core.start();

    SECTION("known profile switches") {
        core.request_endpoint_switch("Stub (unreachable)");
        REQUIRE(wait_until([&] {
            return core.active_endpoint_name() == "Stub (unreachable)";
        }));
        CHECK(fe.calls >= 1);
        CHECK(fe.last_name == "Stub (unreachable)");
        CHECK(fe.last_model == "stub-model");
        CHECK(fe.last_limit == 4242);
    }

    SECTION("missing profile is a no-op bounce") {
        core.request_endpoint_switch("Nonexistent Profile");
        // Give the agent thread a moment to drain the request.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(core.active_endpoint_name().empty());   // unchanged
        CHECK(fe.calls == 0);
    }

    core.stop();
}
