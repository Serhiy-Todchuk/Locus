#include "log_channels.h"

#include <spdlog/spdlog.h>
#include <mutex>

namespace locus {

namespace {

std::mutex g_mu;
std::shared_ptr<spdlog::logger> g_db;
std::shared_ptr<spdlog::logger> g_fs;

std::shared_ptr<spdlog::logger> make_child(const std::string& name)
{
    auto def = spdlog::default_logger();
    if (!def) {
        // Default logger hasn't been installed yet (very early init / tests).
        // Return a no-sink logger so callers never crash; init_logging will
        // install the real sinks shortly. Subsequent calls re-resolve.
        return std::make_shared<spdlog::logger>(name);
    }
    auto& sinks = def->sinks();
    auto lg = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    lg->set_level(spdlog::level::trace);
    lg->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [t:%t] [%^%l%$] %v");
    lg->flush_on(spdlog::level::warn);
    return lg;
}

} // namespace

std::shared_ptr<spdlog::logger> log_db()
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_db || g_db->sinks().empty())
        g_db = make_child("db");
    return g_db;
}

std::shared_ptr<spdlog::logger> log_fs()
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_fs || g_fs->sinks().empty())
        g_fs = make_child("fs");
    return g_fs;
}

void mute_noise_loggers()
{
    log_db()->set_level(spdlog::level::info);
    log_fs()->set_level(spdlog::level::info);
    // Agentic post-mortems read .locus/locus.log between LLM rounds via the
    // `read_locus_log` op. The default flush_on(warn) means most info/trace
    // lines are buffered for minutes -- the harness sees stale content.
    // Drop the flush threshold so every info line lands on disk immediately;
    // trace stays buffered (still gets flushed by warn / on shutdown). The
    // cost is a few extra fsyncs per round -- negligible vs LLM wall-clock.
    spdlog::flush_on(spdlog::level::info);
    log_db()->flush_on(spdlog::level::info);
    log_fs()->flush_on(spdlog::level::info);
    spdlog::info("Noise loggers muted: db, fs (trace -> info); flush_on=info");
}

} // namespace locus
