#pragma once

#include "../file_watcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace locus {

class Indexer;

// Owns a background thread that periodically drains a FileWatcher, batches
// events by quiet period + hard cap, and forwards each batch to
// Indexer::process_events. Replaces the per-frontend pumps that previously
// lived in main.cpp, locus_app.cpp, and locus_frame.cpp.
//
// Thread safety: start()/stop()/flush_now() may be called from any thread, but
// only one thread should drive the pump's lifecycle. The internal drain+process
// path is serialised against external flush_now() callers via mutex_.
class WatcherPump {
public:
    struct Params {
        // Interval between background drain ticks.
        std::chrono::milliseconds tick_interval{250};
        // Flush once no new events have arrived for this long.
        std::chrono::milliseconds quiet_period{1500};
        // Flush regardless once a batch has been accumulating this long.
        std::chrono::milliseconds hard_cap{20000};
    };

    WatcherPump(FileWatcher& watcher, Indexer& indexer, Params params = {});
    ~WatcherPump();

    WatcherPump(const WatcherPump&) = delete;
    WatcherPump& operator=(const WatcherPump&) = delete;

    // Spawn the background thread. Idempotent.
    void start();
    // Signal the thread to exit, flush any pending batch, join. Idempotent.
    void stop();

    // Drain the watcher and process anything pending right now, on the
    // calling thread. Use this before kicking off an agent turn so the LLM
    // sees a fresh index.
    void flush_now();

private:
    void run();

    // Drains `watcher_` into pending_, updates batching timestamps.
    // Caller must hold mutex_.
    void drain_locked();
    // Hands pending_ to the indexer (releases the mutex around the call so
    // a long process_events doesn't block flush_now waiters).
    // Caller must hold mutex_; the lock is released and re-acquired.
    void flush_locked(std::unique_lock<std::mutex>& lock);

    FileWatcher& watcher_;
    Indexer&     indexer_;
    Params       params_;

    std::mutex              mutex_;
    std::condition_variable cv_;
    std::thread             thread_;
    std::atomic<bool>       running_{false};
    bool                    stopping_ = false;

    std::vector<FileEvent>                pending_;
    std::chrono::steady_clock::time_point group_start_{};
    std::chrono::steady_clock::time_point last_event_time_{};
    bool                                  group_active_ = false;
};

} // namespace locus
