#include "embedding_worker.h"
#include "../core/database.h"
#include "../core/log_channels.h"
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
    bool empty_before = false;
    int  enqueued     = static_cast<int>(chunk_ids.size());
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        empty_before = pending_ids_.empty();
        for (auto id : chunk_ids) {
            pending_ids_.push(id);
        }
        total_ += enqueued;
    }
    queue_cv_.notify_one();

    // S5.G -- fire an activity row when a batch lands. Catches the "we just
    // re-indexed ten files; the embedding queue is going to be busy" moment
    // that was previously invisible between "indexing kicked off" and the
    // periodic progress emits below.
    if (enqueued > 0 && on_activity) {
        int total = total_.load();
        int done  = done_.load();
        std::string summary = "Embedding queue: +" + std::to_string(enqueued) +
                              " chunk(s) (" + std::to_string(done) +
                              "/" + std::to_string(total) + ")";
        std::string detail  = "queued=" + std::to_string(enqueued) +
                              " done="  + std::to_string(done) +
                              " total=" + std::to_string(total) +
                              (empty_before ? " (resumed)" : "");
        on_activity(summary, detail);
    }
}

void EmbeddingWorker::set_activity_throttle(int chunks_per_event, int ms_per_event)
{
    activity_chunks_per_event_ = chunks_per_event;
    activity_ms_per_event_     = ms_per_event;
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
    // S6.18 Task A.1 -- prepare the INSERT lazily so a workspace that opens
    // semantic search but never embeds doesn't materialise the 4 MiB-per-table
    // vec0 bucket. S6.18 Task A.5 -- INSERT OR REPLACE handles the rare
    // rowid-reuse race where `chunks.id` (INTEGER PRIMARY KEY without
    // AUTOINCREMENT) is recycled by SQLite after a fast file-rewrite, and a
    // new chunk_vectors row would otherwise collide with a stale embedding.
    sqlite3_stmt* stmt_insert = nullptr;

    while (running_.load()) {
        int64_t chunk_id = -1;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !pending_ids_.empty() || !running_.load();
            });
            if (!running_.load() && pending_ids_.empty()) {
                // S6.18 Task A.6 -- final drain checkpoint on shutdown.
                db_.wal_checkpoint_passive("embedding_worker shutdown");
                break;
            }
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

        // Write vector to chunk_vectors. S6.18 Task A.1: ensure the table
        // exists and lazily prepare the INSERT on first use. S6.18 Task A.5:
        // OR REPLACE so a recycled chunk_id always carries the current
        // chunk's embedding, not a stale one from a freed predecessor.
        if (!stmt_insert) {
            db_.ensure_chunk_vectors_table();
            stmt_insert = db_.prepare(
                "INSERT OR REPLACE INTO chunk_vectors "
                "(chunk_id, embedding) VALUES (?1, ?2)");
        }
        sqlite3_reset(stmt_insert);
        sqlite3_bind_int64(stmt_insert, 1, chunk_id);
        sqlite3_bind_blob(stmt_insert, 2,
                          embedding.data(),
                          static_cast<int>(embedding.size() * sizeof(float)),
                          SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt_insert);
        if (rc != SQLITE_DONE) {
            // With OR REPLACE the UNIQUE-constraint silent-drop path is
            // closed; remaining errors are real (out-of-disk, locked DB,
            // sqlite-vec extension misconfig). Keep warn for visibility.
            spdlog::warn("Failed to insert vector for chunk {}: {}",
                         chunk_id, sqlite3_errmsg(db_.handle()));
        }

        int current_done = ++done_;
        int current_total = total_.load();

        if (on_progress) {
            on_progress(current_done, current_total);
        }

        if (current_done % 100 == 0 || current_done == current_total) {
            log_fs()->trace("Embedding progress: {}/{}", current_done, current_total);
        }

        // S5.G -- throttled activity-row emission. Two independent gates: every
        // N chunks since last emit, OR every M ms since last emit. Drain also
        // fires the last one so the row count matches the total.
        if (on_activity) {
            bool fire = false;
            int  delta = current_done - last_activity_done_;
            if (activity_chunks_per_event_ > 0 && delta >= activity_chunks_per_event_)
                fire = true;
            if (activity_ms_per_event_ > 0) {
                auto now = std::chrono::steady_clock::now();
                if (last_activity_time_.time_since_epoch().count() == 0)
                    last_activity_time_ = now;
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - last_activity_time_).count();
                if (ms >= activity_ms_per_event_)
                    fire = true;
            }
            // Always emit on queue drain so the final state lands in the log.
            if (current_done == current_total && delta > 0)
                fire = true;

            if (fire) {
                std::string summary = "Embedding progress: " +
                                      std::to_string(current_done) + "/" +
                                      std::to_string(current_total) +
                                      (current_done == current_total ? " (done)" : "");
                std::string detail  = "delta=" + std::to_string(delta) +
                                      " done="  + std::to_string(current_done) +
                                      " total=" + std::to_string(current_total);
                on_activity(summary, detail);
                last_activity_done_ = current_done;
                last_activity_time_ = std::chrono::steady_clock::now();
            }
        }

        // S6.18 Task A.6 -- when the queue drains, fold the WAL back into the
        // main DB file. The agentic Tetris vectors.db had a 4.5 MB-per-empty-
        // workspace WAL bloat purely because no caller ever issued a
        // checkpoint between sessions; SQLite's auto-checkpoint kicks in only
        // when WAL crosses ~1000 frames, which a moderate workspace easily
        // exceeds and a tiny one never reaches before clean shutdown.
        if (current_done == current_total && current_total > 0) {
            db_.wal_checkpoint_passive("embedding_worker drain");
        }
    }

    sqlite3_finalize(stmt_read);
    if (stmt_insert) sqlite3_finalize(stmt_insert);
}

} // namespace locus
