#include "file_change_tracker.h"

#include "index/index_query.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace locus {

namespace {
// Same trick as the regex search tool: enumerate the entire indexed tree as a
// flat file list. The huge depth means no path collapses into its parent
// directory entry.
constexpr int k_unbounded_depth = 1'000'000;
} // namespace

void FileChangeTracker::snapshot(IndexQuery& idx)
{
    auto files = idx.list_directory("", k_unbounded_depth);

    std::unordered_map<std::string, int64_t> next;
    next.reserve(files.size());
    for (const auto& fe : files) {
        if (fe.is_directory) continue;
        next.emplace(fe.path, fe.modified_at);
    }

    std::lock_guard lock(mu_);
    snapshot_mtimes_ = std::move(next);
    spdlog::trace("FileChangeTracker: snapshot captured {} files",
                  snapshot_mtimes_.size());
}

FileChangeTracker::Diff
FileChangeTracker::diff_since_snapshot(IndexQuery& idx, std::size_t max_per_bucket) const
{
    auto files = idx.list_directory("", k_unbounded_depth);

    Diff diff;

    std::unordered_map<std::string, int64_t> snap_copy;
    std::unordered_set<std::string>          touched_copy;
    {
        std::lock_guard lock(mu_);
        snap_copy    = snapshot_mtimes_;
        touched_copy = agent_touched_;
    }

    if (snap_copy.empty()) {
        // No previous snapshot -- nothing to diff against. (First turn.)
        return diff;
    }

    std::unordered_set<std::string> seen;
    seen.reserve(files.size());
    for (const auto& fe : files) {
        if (fe.is_directory) continue;
        seen.insert(fe.path);

        if (touched_copy.count(fe.path)) continue;

        auto it = snap_copy.find(fe.path);
        if (it == snap_copy.end()) {
            diff.changed.push_back(fe.path);
        } else if (it->second != fe.modified_at) {
            diff.changed.push_back(fe.path);
        }
    }

    for (const auto& [path, _] : snap_copy) {
        if (touched_copy.count(path)) continue;
        if (!seen.count(path))
            diff.deleted.push_back(path);
    }

    std::sort(diff.changed.begin(), diff.changed.end());
    std::sort(diff.deleted.begin(), diff.deleted.end());

    if (diff.changed.size() > max_per_bucket)
        diff.changed.resize(max_per_bucket);
    if (diff.deleted.size() > max_per_bucket)
        diff.deleted.resize(max_per_bucket);

    return diff;
}

void FileChangeTracker::mark_agent_touched(const std::string& rel_path)
{
    if (rel_path.empty()) return;
    std::lock_guard lock(mu_);
    agent_touched_.insert(rel_path);
}

void FileChangeTracker::clear_agent_touched()
{
    std::lock_guard lock(mu_);
    agent_touched_.clear();
}

std::size_t FileChangeTracker::snapshot_size() const
{
    std::lock_guard lock(mu_);
    return snapshot_mtimes_.size();
}

std::size_t FileChangeTracker::agent_touched_size() const
{
    std::lock_guard lock(mu_);
    return agent_touched_.size();
}

} // namespace locus
