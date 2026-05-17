#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace locus {

// S5.B Terminal Panel sink. Implemented by the GUI's TerminalPanelState
// (rendering sink, S5.R) and LocusFrame (lifecycle-only observer for auto-
// show + busy-badge bookkeeping). Called from worker threads inside
// RunCommandTool (sync execution) and the per-background-process reader
// threads owned by ProcessRegistry.
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

// Thread-safe broker that owns a list of IProcessSink subscribers and
// forwards every emit_* call to each subscriber in registration order.
//
// S5.R turned this into a multi-subscriber broker. Per-tab brokers carry
// 1-2 subscribers each (the tab's TerminalPanelState for rendering plus
// LocusFrame for auto-show / busy-badge bookkeeping); contention is not a
// concern. The empty-subscriber-list state is supported and stays cheap
// (every emit becomes a mutex acquire + empty-vector iterate).
//
// Subscribers must NOT call add_sink / remove_sink from inside an emit_*
// callback -- the emit walks the list under the broker's mutex and any
// re-entry into the broker would deadlock. Subscribers are expected to
// stash events and let a separate thread (typically a UI timer) drain.
class ProcessSinkBroker {
public:
    void add_sink(IProcessSink* s) {
        if (!s) return;
        std::lock_guard l(mu_);
        if (std::find(sinks_.begin(), sinks_.end(), s) == sinks_.end())
            sinks_.push_back(s);
    }
    void remove_sink(IProcessSink* s) {
        std::lock_guard l(mu_);
        sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), s), sinks_.end());
    }

    void emit_bg_started(int id, const std::string& command) {
        std::lock_guard l(mu_);
        for (auto* s : sinks_) s->on_bg_started(id, command);
    }
    void emit_bg_chunk(int id, const char* d, std::size_t n) {
        std::lock_guard l(mu_);
        for (auto* s : sinks_) s->on_bg_chunk(id, d, n);
    }
    void emit_bg_exited(int id, int exit_code, bool killed) {
        std::lock_guard l(mu_);
        for (auto* s : sinks_) s->on_bg_exited(id, exit_code, killed);
    }
    void emit_sync_started(const std::string& command) {
        std::lock_guard l(mu_);
        for (auto* s : sinks_) s->on_sync_started(command);
    }
    void emit_sync_chunk(const char* d, std::size_t n) {
        std::lock_guard l(mu_);
        for (auto* s : sinks_) s->on_sync_chunk(d, n);
    }
    void emit_sync_exited(int exit_code, bool timed_out) {
        std::lock_guard l(mu_);
        for (auto* s : sinks_) s->on_sync_exited(exit_code, timed_out);
    }

    // Subscriber count -- used by tests today. Cheap (mutex + size).
    std::size_t subscriber_count() const {
        std::lock_guard l(mu_);
        return sinks_.size();
    }

private:
    mutable std::mutex          mu_;
    std::vector<IProcessSink*>  sinks_;
};

} // namespace locus
