#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <memory>

namespace efsw { class FileWatcher; class FileWatchListener; }

namespace locus {

namespace fs = std::filesystem;

enum class FileAction {
    Added,
    Modified,
    Deleted,
    Moved
};

struct FileEvent {
    FileAction action;
    fs::path path;          // relative to workspace root
    fs::path old_path;      // only for Moved
};

// Watches a workspace directory for file changes via efsw.
// Events are debounced (200ms) and queued for consumer to drain.
class FileWatcher {
public:
    FileWatcher(const fs::path& root, const std::vector<std::string>& exclude_patterns);
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Start watching (non-blocking, spawns internal thread).
    void start();

    // Stop watching.
    void stop();

    // Drain all pending events into `out`. Returns count drained.
    size_t drain(std::vector<FileEvent>& out);

    // Called by the efsw listener callback (internal use).
    void push_raw(FileAction action, const fs::path& dir, const std::string& filename,
                  const std::string& old_filename);

private:
    bool is_excluded(const fs::path& rel_path) const;

    fs::path root_;
    std::vector<std::string> exclude_patterns_;

    std::unique_ptr<efsw::FileWatcher> watcher_;
    std::unique_ptr<efsw::FileWatchListener> listener_;

    // Debounce state
    struct PendingEvent {
        FileAction action;
        fs::path path;
        fs::path old_path;
        std::chrono::steady_clock::time_point timestamp;
    };
    std::mutex mutex_;
    std::unordered_map<std::string, PendingEvent> pending_;
    std::vector<FileEvent> ready_;
    // Dedupe set for excluded-path trace logging. efsw fires per-event for
    // every excluded file (SQLite WAL flushes on `.locus/vectors.db-wal` can
    // emit several events per second) — we only want to see each excluded
    // path once. Bounded by the number of unique excluded paths encountered.
    std::unordered_set<std::string> excluded_logged_;
};

} // namespace locus
