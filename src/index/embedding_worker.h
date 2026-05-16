#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace locus {

class Database;
class Embedder;

// Background thread that reads chunk text from the DB, runs the embedder,
// and writes vectors into chunk_vectors.  Designed for low-priority background
// work -- does not block the indexer or the UI.
class EmbeddingWorker {
public:
    EmbeddingWorker(Database& db, Embedder& embedder);
    ~EmbeddingWorker();

    EmbeddingWorker(const EmbeddingWorker&) = delete;
    EmbeddingWorker& operator=(const EmbeddingWorker&) = delete;

    void start();
    void stop();

    // Queue chunk IDs for embedding.  Safe to call from any thread.
    void enqueue(std::vector<int64_t> chunk_ids);

    // Embed a single text on the calling thread (for query-time use).
    std::vector<float> embed_query(const std::string& text) const;

    struct Stats {
        int total = 0;
        int done  = 0;
        bool active = false;
    };
    Stats stats() const;

    // Progress callback -- fires on the worker thread.
    std::function<void(int done, int total)> on_progress;

    // S5.G -- coarse activity feed for the agent's activity log. Fires on the
    // worker thread, throttled to N chunks OR M seconds since the last call
    // (whichever first), plus once at queue-drain. The chat-panel/GUI never
    // sees per-chunk noise here; consumers reading at trace level can still
    // tail spdlog. Empty by default.
    std::function<void(const std::string& summary,
                       const std::string& detail)> on_activity;

    // S5.G -- activity-event throttle knobs. Default: every 100 chunks OR 5
    // seconds since the last activity event, whichever fires first. Set
    // chunks=0 to disable the count threshold; ms=0 to disable the time
    // threshold (set both to 0 to emit only at queue drain).
    void set_activity_throttle(int chunks_per_event, int ms_per_event);

private:
    void thread_func();

    Database& db_;
    Embedder& embedder_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<int64_t> pending_ids_;
    std::atomic<int> total_{0};
    std::atomic<int> done_{0};

    // S5.G activity-event throttle.
    int activity_chunks_per_event_ = 100;
    int activity_ms_per_event_     = 5000;
    int last_activity_done_        = 0;
    std::chrono::steady_clock::time_point last_activity_time_{};
};

} // namespace locus
