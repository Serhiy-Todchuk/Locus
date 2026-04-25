#pragma once

#include "conversation.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
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

    // Save with an explicit ID (overwrites if exists).
    void save(const std::string& id, const ConversationHistory& history,
              const nlohmann::json& extras = {});

    // Load a session by ID. Throws std::runtime_error if not found.
    ConversationHistory load(const std::string& id) const;

    // List all saved sessions, sorted newest-first.
    std::vector<SessionInfo> list() const;

    // Delete a session by ID. Returns true if file was removed.
    bool remove(const std::string& id);

    const fs::path& sessions_dir() const { return sessions_dir_; }

private:
    // Generate a timestamp-based ID like "2026-04-14_153042".
    static std::string generate_id();

    // Build session info from a JSON file without loading full history.
    static SessionInfo read_session_info(const fs::path& path);

    fs::path sessions_dir_;
};

} // namespace locus
