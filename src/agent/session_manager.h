#pragma once

#include "conversation.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace locus {

namespace fs = std::filesystem;

// Metadata for a saved session (returned by list_sessions).
struct SessionInfo {
    std::string id;             // Filename stem, e.g. "2026-04-14_153042"
    std::string file_path;      // Absolute path to the JSON file
    int         message_count;  // Number of messages in the session
    int         estimated_tokens;
    std::string first_user_message; // Truncated preview of first user message
    std::string timestamp;      // ISO-ish from the filename
    // S5.I -- user-editable tab title (auto-derived from first user message on
    // first save). Empty for legacy pre-S5.I session files.
    std::string title;
    // S5.I -- unix epoch seconds. created_at is set once when the session file
    // first appears; last_opened_at is bumped on every Open from the menu or
    // Manage Sessions dialog AND on workspace auto-restore. For legacy files
    // missing these fields, both default to the JSON file's mtime.
    long long   created_at      = 0;
    long long   last_opened_at  = 0;
    long long   size_bytes      = 0;
};

// Manages saving/loading conversation sessions to .locus/sessions/.
class SessionManager {
public:
    // sessions_dir is typically workspace/.locus/sessions/
    explicit SessionManager(const fs::path& sessions_dir);

    // Save a conversation to a new session file. Returns the session ID.
    // `extras` is merged into the top-level JSON (e.g. a "metrics" snapshot
    // from MetricsAggregator). Pass an empty/null value to skip.
    std::string save(const ConversationHistory& history,
                     const nlohmann::json& extras = {});

    // Save with an explicit ID (overwrites if exists). `extras` may include
    // a "title" string to persist as the tab title.
    void save(const std::string& id, const ConversationHistory& history,
              const nlohmann::json& extras = {});

    // Load a session by ID. Throws std::runtime_error if not found.
    ConversationHistory load(const std::string& id) const;

    // List all saved sessions, sorted by last_opened_at desc (newest-first).
    std::vector<SessionInfo> list() const;

    // Delete a session by ID. Returns true if file was removed.
    bool remove(const std::string& id);

    // S5.I -- bump the `last_opened_at` field of a session to now. Called when
    // a session is Open'd via the menu / dialog / auto-restore. No-op when the
    // file doesn't exist. Returns true on success.
    bool bump_last_opened_at(const std::string& id);

    // S5.I -- rename: persist a new title into the session file. Returns true
    // on success. The session id (filename stem) does NOT change -- the title
    // is just a display field.
    bool rename(const std::string& id, const std::string& new_title);

    // S5.I -- compute which closed sessions match the auto-cleanup criteria.
    //   keep_last_count   -- protect the N most-recently-opened closed sessions
    //   delete_after_days -- only flag sessions whose last_opened_at is older
    //                        than this many days
    //   currently_open    -- session ids that are open in tabs; never flagged
    //
    // A session is a candidate iff BOTH clauses hold:
    //   * its `last_opened_at` is older than `delete_after_days` ago, AND
    //   * by last_opened_at ordering, it sits beyond the `keep_last_count`
    //     most-recently-opened closed sessions.
    //
    // Returns ids of candidate sessions, oldest-first.
    std::vector<std::string> cleanup_candidates(
        int keep_last_count,
        int delete_after_days,
        const std::unordered_set<std::string>& currently_open = {}) const;

    const fs::path& sessions_dir() const { return sessions_dir_; }

private:
    // Generate a timestamp-based ID like "2026-04-14_153042_a3f2".
    // The 4-char random suffix prevents collisions when two tabs save in the
    // same second (lazy session-file creation can fire concurrently).
    static std::string generate_id();

    // Build session info from a JSON file without loading full history.
    static SessionInfo read_session_info(const fs::path& path);

    // Update a single top-level JSON key in an existing session file, in-place
    // (round-trip parse + dump). Returns true on success.
    bool patch_field(const std::string& id,
                     const std::string& key,
                     const nlohmann::json& value);

    fs::path sessions_dir_;
};

// S5.I -- current time as unix epoch seconds. Exposed for tests so they can
// freeze time without monkey-patching std::chrono.
long long now_epoch_seconds();

} // namespace locus
