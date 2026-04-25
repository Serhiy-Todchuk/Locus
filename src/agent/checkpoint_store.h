#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace locus {

namespace fs = std::filesystem;

// One row of a turn's checkpoint manifest.
struct CheckpointEntry {
    std::string path;       // workspace-relative path (forward slashes)
    std::string action;     // "edit" | "write" | "delete"
    std::string prev_hash;  // sha-1-ish hex of the prior bytes ("" if file did not exist)
    bool        existed;    // whether the file existed before mutation
    bool        skipped;    // true when the file exceeded the size limit
    uint64_t    size_bytes; // size of the prior file (0 if !existed or skipped)
};

// Summary returned by list_turns().
struct TurnInfo {
    std::string session_id;
    int         turn_id;
    std::string created_at;             // ISO-ish string from manifest
    std::vector<CheckpointEntry> entries;
};

// Result of restore_turn().
struct RestoreResult {
    int restored = 0;       // files written back
    int deleted = 0;        // files removed (because they were created in the turn)
    int skipped = 0;        // entries we couldn't restore (large file, missing snapshot, etc.)
    std::vector<std::string> errors;
};

// Persists pre-mutation file snapshots so a whole agent turn can be reverted.
//
// Layout under <workspace>/.locus/checkpoints/:
//   <session_id>/<turn_id>/checkpoint.json   manifest (entries[])
//   <session_id>/<turn_id>/files/<rel_path>  byte-identical pre-mutation copies
//
// Files larger than max_file_size_bytes() are recorded in the manifest with
// `skipped=true` and no body — defer to git for those.
//
// Thread-safety: snapshot_before / finalize_turn / restore_turn / gc may be
// called from any thread. The mutex serializes manifest IO; file IO under the
// per-turn directory does not collide because each turn is a fresh subdir.
class CheckpointStore {
public:
    // checkpoints_dir is typically <workspace>/.locus/checkpoints/.
    explicit CheckpointStore(fs::path checkpoints_dir);

    // 1 MB default — anything bigger is too costly to copy on every edit and
    // is better handled by git (per the stage spec).
    void set_max_file_size_bytes(uint64_t v) { max_file_size_bytes_ = v; }
    uint64_t max_file_size_bytes() const { return max_file_size_bytes_; }

    // Default 50 — keep the most-recent N turns per session, prune older.
    void set_max_turns_per_session(int n) { max_turns_per_session_ = n; }
    int max_turns_per_session() const { return max_turns_per_session_; }

    // Take a pre-mutation snapshot of `abs_path` (which must already be inside
    // workspace_root). `action` is "edit" | "write" | "delete". If the file
    // doesn't exist yet, an entry is still recorded with existed=false so the
    // restore step can delete the freshly-created file.
    //
    // Idempotent within a turn: a second call for the same (session, turn,
    // path) is a no-op so a tool that touches the same file twice doesn't
    // overwrite the original snapshot.
    //
    // Returns true on success. Logs and returns false on filesystem errors —
    // the tool call should still proceed (we don't want checkpointing to
    // block real work).
    bool snapshot_before(const std::string& session_id,
                         int turn_id,
                         const fs::path& workspace_root,
                         const fs::path& abs_path,
                         const std::string& action);

    // Restore every file recorded in (session_id, turn_id) back to its
    // pre-mutation state. Files that were created during the turn (existed=
    // false) are deleted. Returns counts + per-file errors.
    RestoreResult restore_turn(const std::string& session_id,
                               int turn_id,
                               const fs::path& workspace_root) const;

    // Drop a turn's snapshots. Returns true if the directory existed.
    bool drop_turn(const std::string& session_id, int turn_id);

    // Run GC for one session: keep the most-recent max_turns_per_session,
    // delete the rest. Returns the number of pruned turn directories.
    int gc_session(const std::string& session_id);

    // Drop everything for a session (called on explicit session close /
    // delete). Returns true if the session dir existed.
    bool drop_session(const std::string& session_id);

    // Read-only inspection used by the GUI (per-turn revert UI).
    std::vector<TurnInfo> list_turns(const std::string& session_id) const;
    std::optional<TurnInfo> read_turn(const std::string& session_id,
                                      int turn_id) const;

    // Number of mutation entries (skipped or not) in this turn. 0 means the
    // turn touched no files and the manifest doesn't exist yet.
    int entry_count(const std::string& session_id, int turn_id) const;

    const fs::path& root() const { return root_; }

private:
    fs::path turn_dir(const std::string& session_id, int turn_id) const;
    fs::path manifest_path(const std::string& session_id, int turn_id) const;
    fs::path session_dir(const std::string& session_id) const;

    // Lower-level helpers used by snapshot_before() — kept separate so they
    // can be unit-tested in isolation.
    static std::string hash_file(const fs::path& p);
    static std::string make_relative(const fs::path& workspace_root,
                                     const fs::path& abs_path);

    fs::path root_;
    uint64_t max_file_size_bytes_ = 1024ull * 1024ull;  // 1 MB
    int      max_turns_per_session_ = 50;

    mutable std::mutex io_mutex_;
};

} // namespace locus
