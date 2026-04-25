#include "shared_embedder.h"

#include "embedder.h"
#include "workspace.h"

#include <catch2/catch_session.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace fs = std::filesystem;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace locus::test {

namespace {

// 384-dim, English-only, ~37 MB on disk — Workspace tests don't care about
// quality, only about exercising the semantic-search code path, so the
// smallest reasonable bi-encoder wins.
const std::string k_model_filename = "bge-small-en-v1.5-Q8_0.gguf";

std::mutex                g_mu;
std::shared_ptr<Embedder> g_embedder;

fs::path resolve_model_path()
{
    // Mirror Workspace's resolution: walk up from the exe dir looking for
    // models/<filename>. Tests run from build/<cfg>/tests/<cfg>/, so the
    // models/ directory at the repo root is 4–5 levels up.
    fs::path exe_dir = fs::current_path();
#ifdef _WIN32
    {
        wchar_t buf[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, buf, MAX_PATH))
            exe_dir = fs::path(buf).parent_path();
    }
#endif
    fs::path base = exe_dir;
    for (int i = 0; i < 6; ++i) {
        fs::path candidate = base / "models" / k_model_filename;
        if (fs::exists(candidate)) return candidate;
        if (!base.has_parent_path() || base.parent_path() == base) break;
        base = base.parent_path();
    }
    return {};
}

} // namespace

std::shared_ptr<Embedder> SharedTestEmbedder::get()
{
    std::lock_guard lock(g_mu);
    if (g_embedder) return g_embedder;

    auto path = resolve_model_path();
    if (path.empty())
        throw std::runtime_error(
            "SharedTestEmbedder: '" + k_model_filename + "' not found under "
            "any 'models/' directory walking up from the test exe. Run "
            "models/download-small.ps1.");

    spdlog::info("SharedTestEmbedder: loading {}", path.string());
    g_embedder = std::make_shared<Embedder>(path);
    return g_embedder;
}

void SharedTestEmbedder::shutdown()
{
    std::lock_guard lock(g_mu);
    g_embedder.reset();
}

const std::string& SharedTestEmbedder::model_filename()
{
    return k_model_filename;
}

namespace {

// Catch2 listener: install the provider before any test runs and tear it
// down before the process exits. Doing this in a listener (rather than at
// static-init time) keeps the load lazy — tests that never touch semantic
// search never trigger the model load.
class EmbedderListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        Workspace::set_embedder_provider([] {
            return SharedTestEmbedder::get();
        });
    }

    void testRunEnded(const Catch::TestRunStats&) override
    {
        Workspace::set_embedder_provider(nullptr);
        SharedTestEmbedder::shutdown();
    }
};

} // namespace

} // namespace locus::test

CATCH_REGISTER_LISTENER(locus::test::EmbedderListener)
