#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace locus {

class IndexQuery;

// Tracks the workspace's indexed-file state between turns so the agent can
// be told which files changed under it (S4.T).
//
// Lifecycle (driven by AgentCore):
//   - construct once at AgentCore startup; take an initial snapshot.
//   - end of every turn:
//       * snapshot() to capture mtimes for the diff next turn.
//   - start of every turn (after snapshot was taken last turn):
//       * diff_since_snapshot() returns the changed paths.
//       * clear_agent_touched() resets the suppression set for the new turn.
//   - whenever the agent runs a mutating tool:
//       * mark_agent_touched(path) so its own writes don't echo back.
//
// All paths are workspace-relative, matching IndexQuery::list_directory output.
//
// Thread safety: every method takes an internal mutex. AgentCore drives diff/
// snapshot from the agent thread, while ToolDispatcher may call
// mark_agent_touched from the same thread; the lock is cheap and keeps the
// contract obvious.
class FileChangeTracker {
public:
    struct Diff {
        std::vector<std::string> changed; // added or modified (mtime differs)
        std::vector<std::string> deleted; // present in snapshot, gone now
    };

    FileChangeTracker() = default;

    // Capture the current (path -> mtime) map from the index. Replaces any
    // previous snapshot. Cheap (single sqlite scan).
    void snapshot(IndexQuery& idx);

    // Compare the current index state against the last snapshot. Paths in the
    // agent-touched set are filtered out so the agent's own edits don't bounce
    // back at it. Returns paths in lexicographic order, capped per-bucket.
    Diff diff_since_snapshot(IndexQuery& idx, std::size_t max_per_bucket = 50) const;

    // Record a path the agent itself just mutated (edit_file / write_file /
    // delete_file). Matched against the diff next turn.
    void mark_agent_touched(const std::string& rel_path);

    // Reset the agent-touched set (called at the start of each user turn).
    void clear_agent_touched();

    // Test/inspection: how many files we last snapshotted, how many we'd
    // currently exclude.
    std::size_t snapshot_size() const;
    std::size_t agent_touched_size() const;

private:
    mutable std::mutex                       mu_;
    std::unordered_map<std::string, int64_t> snapshot_mtimes_;
    std::unordered_set<std::string>          agent_touched_;
};

} // namespace locus
