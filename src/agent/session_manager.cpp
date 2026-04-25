#include "session_manager.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace locus {

using json = nlohmann::json;

SessionManager::SessionManager(const fs::path& sessions_dir)
    : sessions_dir_(sessions_dir)
{
    std::error_code ec;
    fs::create_directories(sessions_dir_, ec);
    if (ec)
        spdlog::warn("SessionManager: cannot create sessions dir: {}", ec.message());
}

// -- Generate ID --------------------------------------------------------------

std::string SessionManager::generate_id()
{
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time_t);
#else
    localtime_r(&time_t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H%M%S");
    return oss.str();
}

// -- Save ---------------------------------------------------------------------

std::string SessionManager::save(const ConversationHistory& history,
                                 const nlohmann::json& extras)
{
    std::string id = generate_id();
    save(id, history, extras);
    return id;
}

void SessionManager::save(const std::string& id,
                          const ConversationHistory& history,
                          const nlohmann::json& extras)
{
    auto path = sessions_dir_ / (id + ".json");

    json j;
    j["id"] = id;
    j["message_count"] = static_cast<int>(history.size());
    j["estimated_tokens"] = history.estimate_tokens();
    j["messages"] = history.to_json();

    // Merge top-level extras (e.g. "metrics") in last so saved metadata can
    // co-exist with future session-level fields without schema bumps.
    if (extras.is_object()) {
        for (auto it = extras.begin(); it != extras.end(); ++it)
            j[it.key()] = it.value();
    }

    // Extract first user message for preview.
    for (auto& msg : history.messages()) {
        if (msg.role == MessageRole::user) {
            auto preview = msg.content.substr(0, 120);
            if (msg.content.size() > 120) preview += "...";
            j["first_user_message"] = preview;
            break;
        }
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("SessionManager: cannot write {}", path.string());
        throw std::runtime_error("Cannot write session file: " + path.string());
    }

    f << j.dump(2) << '\n';
    spdlog::info("SessionManager: saved session '{}' ({} messages)",
                 id, history.size());
}

// -- Load ---------------------------------------------------------------------

ConversationHistory SessionManager::load(const std::string& id) const
{
    auto path = sessions_dir_ / (id + ".json");

    if (!fs::exists(path))
        throw std::runtime_error("Session not found: " + id);

    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot read session file: " + path.string());

    json j = json::parse(f);

    if (!j.contains("messages"))
        throw std::runtime_error("Invalid session file (no messages): " + id);

    auto history = ConversationHistory::from_json(j["messages"]);
    spdlog::info("SessionManager: loaded session '{}' ({} messages)",
                 id, history.size());
    return history;
}

// -- List ---------------------------------------------------------------------

SessionInfo SessionManager::read_session_info(const fs::path& path)
{
    SessionInfo info;
    info.file_path = path.string();
    info.id = path.stem().string();
    info.timestamp = info.id;  // ID is the timestamp

    try {
        std::ifstream f(path);
        if (!f.is_open()) return info;

        json j = json::parse(f);
        info.message_count = j.value("message_count", 0);
        info.estimated_tokens = j.value("estimated_tokens", 0);
        info.first_user_message = j.value("first_user_message", "");
    } catch (const json::exception& e) {
        spdlog::warn("SessionManager: cannot read session info from {}: {}",
                     path.string(), e.what());
    }

    return info;
}

std::vector<SessionInfo> SessionManager::list() const
{
    std::vector<SessionInfo> sessions;

    if (!fs::exists(sessions_dir_))
        return sessions;

    for (auto& entry : fs::directory_iterator(sessions_dir_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        sessions.push_back(read_session_info(entry.path()));
    }

    // Sort newest-first (ID is a timestamp string, lexicographic sort works).
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionInfo& a, const SessionInfo& b) {
                  return a.id > b.id;
              });

    spdlog::trace("SessionManager: listed {} sessions", sessions.size());
    return sessions;
}

// -- Remove -------------------------------------------------------------------

bool SessionManager::remove(const std::string& id)
{
    auto path = sessions_dir_ / (id + ".json");
    std::error_code ec;
    bool removed = fs::remove(path, ec);
    if (removed)
        spdlog::info("SessionManager: removed session '{}'", id);
    return removed;
}

} // namespace locus
