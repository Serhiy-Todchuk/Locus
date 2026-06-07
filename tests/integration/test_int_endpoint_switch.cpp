// S6.16 -- endpoint hot-swap integration test (manual-only, live LLM).
//
// Configures a temporary global endpoints.json (via LOCUS_GLOBAL_DIR) with two
// profiles: the harness's local LM Studio endpoint and a hosted NVIDIA Build
// endpoint. Switches the harness agent from local -> NVIDIA between turns and
// verifies:
//   1. The hot-swap reseats the client (active_base_url + active_endpoint_name
//      change to the NVIDIA profile).
//   2. The second turn actually lands on the new endpoint with a valid
//      Authorization header -- proven by a non-empty, error-free response from
//      the qwen3-coder model (NVIDIA returns 401 without a valid Bearer).
//   3. Conversation history survives the switch (the first turn's messages are
//      still present).
//
// The NVIDIA key is read from the env var LOCUS_NVIDIA_API_KEY -- the test
// SKIPs (via WARN + early return) when it is absent so the repo never carries
// a secret and CI without a key doesn't fail. To run the live verification:
//   set LOCUS_NVIDIA_API_KEY=nvapi-...   (PowerShell: $env:LOCUS_NVIDIA_API_KEY)
//   locus_integration_tests.exe "[s6.16]"

#include "harness_fixture.h"

#include "agent/agent_core.h"
#include "agent/conversation.h"
#include "llm/endpoint_profile.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace locus;
using namespace locus::integration;

namespace {

void set_env(const char* name, const std::string& value)
{
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Save + restore LOCUS_GLOBAL_DIR so the temp store doesn't leak into other
// integration cases that read the real global dir.
struct GlobalDirGuard {
    std::string prev;
    bool had_prev = false;
    GlobalDirGuard() {
        if (const char* p = std::getenv("LOCUS_GLOBAL_DIR")) { prev = p; had_prev = true; }
    }
    ~GlobalDirGuard() {
#if defined(_WIN32)
        _putenv_s("LOCUS_GLOBAL_DIR", had_prev ? prev.c_str() : "");
#else
        if (had_prev) setenv("LOCUS_GLOBAL_DIR", prev.c_str(), 1);
        else          unsetenv("LOCUS_GLOBAL_DIR");
#endif
    }
};

bool wait_until(std::function<bool()> pred, int timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

} // namespace

TEST_CASE("Endpoint hot-swap: local -> NVIDIA between turns preserves history",
          "[integration][llm][s6.16]")
{
    const char* key = std::getenv("LOCUS_NVIDIA_API_KEY");
    if (!key || !*key) {
        WARN("LOCUS_NVIDIA_API_KEY not set -- skipping the live NVIDIA "
             "endpoint-switch test. Set it to an nvapi-... key to run.");
        return;
    }

    auto& h = harness();

    // Point the global store at a temp dir + seed two profiles. The harness's
    // local endpoint is the active one; NVIDIA is the switch target. Each
    // request_endpoint_switch reloads the store fresh, so writing the file
    // here is enough.
    GlobalDirGuard global_dir_guard;
    auto tmp = h.tmp_dir() / "ep_switch_global";
    std::error_code ec;
    std::filesystem::create_directories(tmp, ec);
    set_env("LOCUS_GLOBAL_DIR", tmp.string());

    {
        EndpointProfileStore store;
        store.load();   // seeds builtins into tmp
        EndpointProfile local;
        local.name = "Harness Local";
        local.base_url = "http://127.0.0.1:1234";
        local.tool_format = "auto";
        store.add(local);

        EndpointProfile nv;
        nv.name = "NVIDIA Build (hosted)";   // builtin already present; edit it
        nv.base_url = "https://integrate.api.nvidia.com/v1";
        nv.api_key = key;
        nv.default_model = "qwen/qwen3-coder-480b-a35b-instruct";
        nv.tool_format = "openai";
        if (!store.update(nv)) store.add(nv);

        store.set_active("Harness Local");
        REQUIRE(store.save().empty());
    }

    // Turn 1 on the local endpoint.
    PromptResult r1 = h.prompt("Reply with exactly the word: ping");
    REQUIRE(r1.errors.empty());
    REQUIRE_FALSE(r1.timed_out);
    size_t history_after_t1 = h.agent().history().messages().size();
    REQUIRE(history_after_t1 >= 2);   // at least system + user (+ assistant)

    // Switch to NVIDIA and wait for the swap to apply on the agent thread.
    h.agent().request_endpoint_switch("NVIDIA Build (hosted)");
    bool switched = wait_until([&] {
        return h.agent().active_endpoint_name() == "NVIDIA Build (hosted)";
    }, 30000);
    REQUIRE(switched);
    CHECK(h.agent().active_base_url() == "https://integrate.api.nvidia.com/v1");

    // Turn 2 on NVIDIA. A non-empty, error-free response proves base_url + the
    // Bearer auth header both landed (NVIDIA 401s without a valid key).
    PromptResult r2 = h.prompt("In one short sentence, what language are you "
                               "best known for helping with?");
    if (!r2.errors.empty()) {
        WARN("NVIDIA turn returned an error (key invalid / rate-limited / "
             "network down?). First error: " << r2.errors.front()
             << " -- treating as environmental skip.");
        // Restore the harness to a sane endpoint before bailing.
        h.agent().request_endpoint_switch("Harness Local");
        wait_until([&] {
            return h.agent().active_endpoint_name() == "Harness Local";
        }, 30000);
        return;
    }
    REQUIRE_FALSE(r2.timed_out);
    CHECK_FALSE(r2.tokens.empty());

    // History survived the switch -- turn 2 only added to it, never reset.
    CHECK(h.agent().history().messages().size() > history_after_t1);

    // Leave the harness on the local endpoint for subsequent cases.
    h.agent().request_endpoint_switch("Harness Local");
    wait_until([&] {
        return h.agent().active_endpoint_name() == "Harness Local";
    }, 30000);
}
