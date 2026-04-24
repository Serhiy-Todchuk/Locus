#include "file_watcher.h"
#include "glob_match.h"

#include <spdlog/spdlog.h>
#include <efsw/efsw.hpp>

#include <algorithm>

namespace locus {

// -- efsw listener ------------------------------------------------------------

class Listener : public efsw::FileWatchListener {
public:
    explicit Listener(FileWatcher* owner) : owner_(owner) {}

    void handleFileAction(efsw::WatchID /*id*/,
                          const std::string& dir,
                          const std::string& filename,
                          efsw::Action action,
                          std::string oldFilename) override
    {
        FileAction fa;
        switch (action) {
            case efsw::Actions::Add:      fa = FileAction::Added;    break;
            case efsw::Actions::Delete:    fa = FileAction::Deleted;  break;
            case efsw::Actions::Modified:  fa = FileAction::Modified; break;
            case efsw::Actions::Moved:     fa = FileAction::Moved;    break;
            default: return;
        }
        owner_->push_raw(fa, fs::path(dir), filename, oldFilename);
    }

private:
    FileWatcher* owner_;
};

// -- FileWatcher implementation -----------------------------------------------

FileWatcher::FileWatcher(const fs::path& root, const std::vector<std::string>& exclude_patterns)
    : root_(root)
    , exclude_patterns_(exclude_patterns)
    , watcher_(std::make_unique<efsw::FileWatcher>())
    , listener_(std::make_unique<Listener>(this))
{
}

FileWatcher::~FileWatcher()
{
    stop();
}

void FileWatcher::start()
{
    watcher_->addWatch(root_.string(), listener_.get(), true /* recursive */);
    watcher_->watch();
    spdlog::info("File watcher started on {}", root_.string());
}

void FileWatcher::stop()
{
    // efsw::FileWatcher destructor stops the watch thread
    spdlog::trace("File watcher stopping");
}

bool FileWatcher::is_excluded(const fs::path& rel_path) const
{
    std::string rel_str = rel_path.string();
    std::replace(rel_str.begin(), rel_str.end(), '\\', '/');

    for (auto& pattern : exclude_patterns_) {
        if (glob_match(pattern, rel_str)) {
            return true;
        }
    }
    return false;
}

void FileWatcher::push_raw(FileAction action, const fs::path& dir,
                            const std::string& filename,
                            const std::string& old_filename)
{
    fs::path abs_path = fs::path(dir) / filename;
    fs::path rel_path = fs::relative(abs_path, root_);

    if (is_excluded(rel_path)) {
        // Log each excluded path at most once — otherwise SQLite WAL flushes
        // on .locus/vectors.db-wal turn the trace log into a firehose.
        std::string rel_str = rel_path.string();
        std::lock_guard<std::mutex> lock(mutex_);
        if (excluded_logged_.insert(rel_str).second) {
            spdlog::trace("File watcher: excluded {} (further events suppressed)",
                          rel_str);
        }
        return;
    }

    spdlog::trace("File watcher: {} {}", static_cast<int>(action), rel_path.string());

    std::lock_guard<std::mutex> lock(mutex_);

    auto key = rel_path.string();
    auto now = std::chrono::steady_clock::now();

    PendingEvent pe;
    pe.action = action;
    pe.path = rel_path;
    pe.timestamp = now;

    if (action == FileAction::Moved) {
        fs::path old_abs = fs::path(dir) / old_filename;
        pe.old_path = fs::relative(old_abs, root_);
    }

    pending_[key] = pe;
}

size_t FileWatcher::drain(std::vector<FileEvent>& out)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    constexpr auto debounce_ms = std::chrono::milliseconds(200);

    size_t count = 0;
    auto it = pending_.begin();
    while (it != pending_.end()) {
        if (now - it->second.timestamp >= debounce_ms) {
            FileEvent ev;
            ev.action = it->second.action;
            ev.path = it->second.path;
            ev.old_path = it->second.old_path;
            out.push_back(std::move(ev));
            it = pending_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }

    return count;
}

} // namespace locus
