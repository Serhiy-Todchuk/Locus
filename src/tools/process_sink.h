#pragma once

#include <cstddef>
#include <mutex>
#include <string>

namespace locus {

// S5.B Terminal Panel sink. Implemented by the GUI's TerminalPanel; called
// from worker threads inside RunCommandTool (sync execution) and the per-
// background-process reader threads owned by ProcessRegistry.
//
// Implementations MUST be thread-safe -- chunks fire from arbitrary threads,
// often at very high rates (parallel compiles spam stderr). The expected
// shape is: take a short mutex, append into a per-id pending buffer; let a
// UI-thread timer drain and render. Do not call into wx widgets here.
class IProcessSink {
public:
    virtual ~IProcessSink() = default;

    // Background process lifecycle (ids from `ProcessRegistry::spawn`).
    virtual void on_bg_started(int id, const std::string& command) = 0;
    virtual void on_bg_chunk  (int id, const char* data, std::size_t n) = 0;
    virtual void on_bg_exited (int id, int exit_code, bool killed) = 0;

    // Synchronous `run_command` invocations. At most one in flight at a time;
    // panels typically reserve a single fixed tab and clear it on each
    // on_sync_started.
    virtual void on_sync_started(const std::string& command) = 0;
    virtual void on_sync_chunk  (const char* data, std::size_t n) = 0;
    virtual void on_sync_exited (int exit_code, bool timed_out) = 0;
};

// Tiny thread-safe broker that owns an IProcessSink pointer and forwards
// calls under a mutex. ProcessRegistry holds one of these; the GUI calls
// `set_sink` once at startup to attach the TerminalPanel.
//
// Null sink is the default and the supported steady state for CLI / headless
// runs -- every emit_* is a cheap mutex + null check.
class ProcessSinkBroker {
public:
    void set_sink(IProcessSink* s) { std::lock_guard l(mu_); sink_ = s; }

    void emit_bg_started(int id, const std::string& command) {
        std::lock_guard l(mu_); if (sink_) sink_->on_bg_started(id, command);
    }
    void emit_bg_chunk(int id, const char* d, std::size_t n) {
        std::lock_guard l(mu_); if (sink_) sink_->on_bg_chunk(id, d, n);
    }
    void emit_bg_exited(int id, int exit_code, bool killed) {
        std::lock_guard l(mu_); if (sink_) sink_->on_bg_exited(id, exit_code, killed);
    }
    void emit_sync_started(const std::string& command) {
        std::lock_guard l(mu_); if (sink_) sink_->on_sync_started(command);
    }
    void emit_sync_chunk(const char* d, std::size_t n) {
        std::lock_guard l(mu_); if (sink_) sink_->on_sync_chunk(d, n);
    }
    void emit_sync_exited(int exit_code, bool timed_out) {
        std::lock_guard l(mu_); if (sink_) sink_->on_sync_exited(exit_code, timed_out);
    }

private:
    std::mutex     mu_;
    IProcessSink*  sink_ = nullptr;
};

} // namespace locus
