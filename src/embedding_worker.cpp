#include "embedding_worker.h"
#include "database.h"
#include "embedder.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

namespace locus {

EmbeddingWorker::EmbeddingWorker(Database& db, Embedder& embedder)
    : db_(db)
    , embedder_(embedder)
{
}

EmbeddingWorker::~EmbeddingWorker()
{
    stop();
}

void EmbeddingWorker::start()
{
    if (running_.exchange(true)) return;  // already running
    thread_ = std::thread(&EmbeddingWorker::thread_func, this);
    spdlog::info("Embedding worker started");
}

void EmbeddingWorker::stop()
{
    if (!running_.exchange(false)) return;
    queue_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    spdlog::info("Embedding worker stopped");
}

void EmbeddingWorker::enqueue(std::vector<int64_t> chunk_ids)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (auto id : chunk_ids) {
            pending_ids_.push(id);
        }
        total_ += static_cast<int>(chunk_ids.size());
    }
    queue_cv_.notify_one();
}

std::vector<float> EmbeddingWorker::embed_query(const std::string& text) const
{
    return embedder_.embed(text);
}

EmbeddingWorker::Stats EmbeddingWorker::stats() const
{
    Stats s;
    s.total = total_.load();
    s.done = done_.load();
    s.active = running_.load() && (s.done < s.total);
    return s;
}

void EmbeddingWorker::thread_func()
{
    // Prepared statements for this thread (SQLite statements are not thread-safe)
    sqlite3_stmt* stmt_read = db_.prepare(
        "SELECT content FROM chunks WHERE id = ?1");
    sqlite3_stmt* stmt_insert = db_.prepare(
        "INSERT INTO chunk_vectors (chunk_id, embedding) VALUES (?1, ?2)");

    while (running_.load()) {
        int64_t chunk_id = -1;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !pending_ids_.empty() || !running_.load();
            });
            if (!running_.load() && pending_ids_.empty()) break;
            if (pending_ids_.empty()) continue;

            chunk_id = pending_ids_.front();
            pending_ids_.pop();
        }

        // Read chunk text
        sqlite3_reset(stmt_read);
        sqlite3_bind_int64(stmt_read, 1, chunk_id);
        int rc = sqlite3_step(stmt_read);
        if (rc != SQLITE_ROW) {
            spdlog::warn("Embedding worker: chunk {} not found", chunk_id);
            ++done_;
            continue;
        }

        const char* text = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt_read, 0));
        if (!text) {
            ++done_;
            continue;
        }

        std::string content(text);

        // Run embedding
        std::vector<float> embedding;
        try {
            embedding = embedder_.embed(content);
        } catch (const std::exception& e) {
            spdlog::warn("Embedding failed for chunk {}: {}", chunk_id, e.what());
            ++done_;
            continue;
        }

        // Write vector to chunk_vectors
        sqlite3_reset(stmt_insert);
        sqlite3_bind_int64(stmt_insert, 1, chunk_id);
        sqlite3_bind_blob(stmt_insert, 2,
                          embedding.data(),
                          static_cast<int>(embedding.size() * sizeof(float)),
                          SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt_insert);
        if (rc != SQLITE_DONE) {
            spdlog::warn("Failed to insert vector for chunk {}: {}",
                         chunk_id, sqlite3_errmsg(db_.handle()));
        }

        int current_done = ++done_;
        int current_total = total_.load();

        if (on_progress) {
            on_progress(current_done, current_total);
        }

        if (current_done % 100 == 0 || current_done == current_total) {
            spdlog::trace("Embedding progress: {}/{}", current_done, current_total);
        }
    }

    sqlite3_finalize(stmt_read);
    sqlite3_finalize(stmt_insert);
}

} // namespace locus
