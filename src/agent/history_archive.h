#pragma once

#include "conversation.h"

#include <filesystem>
#include <string>
#include <vector>

namespace locus {

// S5.F -- Chained backups of the conversation history written before every
// compaction. Layout:
//
//   <sessions_dir>/<session_id>/history.before-compact-<N>.json
//
// <N> is a monotonic counter per session, starting at 1. The current live
// session JSON (saved by SessionManager) lives at <sessions_dir>/<id>.json
// and is never touched by this class -- the archive is a parallel write
// trail intended for manual recovery (open the JSON, copy back what you
// want). The LLM does NOT get a tool to read these archives in v1; that's
// a deliberate choice (see S5.F doc).
//
// HistoryArchive owns no state -- it's a thin filesystem helper. The
// session id is supplied per call so a single instance can serve all the
// active sessions for a workspace.
class HistoryArchive {
public:
    explicit HistoryArchive(std::filesystem::path sessions_dir);

    // Write a snapshot. Returns the path written, or an empty path if no
    // snapshot was written (e.g. directory creation failed). Computes the
    // next counter by scanning existing archives. Safe to call on a session
    // that has never been archived before.
    std::filesystem::path snapshot(const std::string& session_id,
                                    const ConversationHistory& history);

    // Prune older snapshots so at most `keep_count` files remain in the
    // session's archive directory. No-op when keep_count <= 0 or fewer
    // snapshots exist.
    void gc(const std::string& session_id, int keep_count);

    // List the archive files for `session_id`, sorted by counter ascending.
    std::vector<std::filesystem::path> list(const std::string& session_id) const;

    // The directory this archive writes into. Public for tests that need
    // to inspect on-disk layout.
    const std::filesystem::path& root() const { return sessions_dir_; }

    // Parse the counter out of a filename of the form
    // "history.before-compact-<N>.json". Returns 0 on no match (callers
    // should ignore those files).
    static int counter_from_filename(const std::string& filename);

    // Build the relative-from-workspace path used for the footnote that
    // gets injected into the compacted history.
    static std::string footnote_relative_path(const std::string& session_id,
                                              int counter);

private:
    std::filesystem::path session_dir(const std::string& session_id) const;

    std::filesystem::path sessions_dir_;
};

} // namespace locus
