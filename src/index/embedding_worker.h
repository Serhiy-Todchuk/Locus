#pragma once

#include <atomic>
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
// work — does not block the indexer or the UI.
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

    // Progress callback — fires on the worker thread.
    std::function<void(int done, int total)> on_progress;

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
};

} // namespace locus
