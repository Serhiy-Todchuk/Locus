#include "watcher_pump.h"
#include "../indexer.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace locus {

WatcherPump::WatcherPump(FileWatcher& watcher, Indexer& indexer, Params params)
    : watcher_(watcher), indexer_(indexer), params_(params)
{}

WatcherPump::~WatcherPump()
{
    stop();
}

void WatcherPump::start()
{
    if (running_.exchange(true)) return;
    stopping_ = false;
    thread_ = std::thread(&WatcherPump::run, this);
}

void WatcherPump::stop()
{
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();

    // Final synchronous flush so edits made right before shutdown still land.
    try {
        flush_now();
    } catch (const std::exception& ex) {
        spdlog::warn("WatcherPump final flush failed: {}", ex.what());
    }
}

void WatcherPump::flush_now()
{
    std::unique_lock<std::mutex> lock(mutex_);
    drain_locked();
    if (!pending_.empty())
        flush_locked(lock);
}

void WatcherPump::run()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        drain_locked();

        if (group_active_) {
            auto now   = std::chrono::steady_clock::now();
            auto idle  = now - last_event_time_;
            auto total = now - group_start_;
            if (idle >= params_.quiet_period || total >= params_.hard_cap)
                flush_locked(lock);
        }

        cv_.wait_for(lock, params_.tick_interval, [this] { return stopping_; });
    }
}

void WatcherPump::drain_locked()
{
    std::vector<FileEvent> drained;
    size_t n = watcher_.drain(drained);
    if (n == 0) return;

    auto now = std::chrono::steady_clock::now();
    if (!group_active_) {
        group_start_  = now;
        group_active_ = true;
    }
    last_event_time_ = now;

    pending_.insert(pending_.end(),
                    std::make_move_iterator(drained.begin()),
                    std::make_move_iterator(drained.end()));
    spdlog::trace("WatcherPump: +{} events (pending {})", n, pending_.size());
}

void WatcherPump::flush_locked(std::unique_lock<std::mutex>& lock)
{
    if (pending_.empty()) {
        group_active_ = false;
        return;
    }

    std::vector<FileEvent> batch = std::move(pending_);
    pending_.clear();
    group_active_ = false;

    // Release the mutex while indexing so flush_now() callers and the pump
    // thread don't block each other on long batches. process_events takes
    // its own internal lock to stay thread-safe.
    lock.unlock();
    spdlog::info("WatcherPump: flushing {} batched event(s)", batch.size());
    try {
        indexer_.process_events(batch);
    } catch (const std::exception& ex) {
        spdlog::error("WatcherPump: process_events failed: {}", ex.what());
    }
    lock.lock();
}

} // namespace locus
