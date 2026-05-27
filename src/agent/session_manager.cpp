#include "session_manager.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace locus {

using json = nlohmann::json;

// -- Time helpers -------------------------------------------------------------

long long now_epoch_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace {

// Best-effort title derivation from a message preview. Used on legacy sessions
// (no `title` field) and on the first save of a brand-new tab.
std::string derive_title_from_preview(const std::string& preview)
{
    std::string title;
    title.reserve(preview.size());
    for (char c : preview) {
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!title.empty() && title.back() != ' ') title.push_back(' ');
        } else {
            title.push_back(c);
        }
    }
    // Trim
    auto not_space = [](unsigned char c) { return c != ' '; };
    auto first = std::find_if(title.begin(), title.end(), not_space);
    auto last  = std::find_if(title.rbegin(), title.rend(), not_space).base();
    if (first >= last) return {};
    title.assign(first, last);
    if (title.size() > 60) {
        title.resize(57);
        title += "...";
    }
    return title;
}

} // namespace

// -- Construction -------------------------------------------------------------

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

    // 4-char hex suffix avoids same-second collisions when two tabs save
    // their first user message concurrently. Random rather than monotonic
    // because the SessionManager has no shared counter across instances.
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 0xFFFF);
    int suffix = dist(rng);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H%M%S")
        << '_' << std::hex << std::setw(4) << std::setfill('0') << suffix;
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

    // S5.I -- created_at preserved on overwrite; last_opened_at bumped to now.
    long long now_s = now_epoch_seconds();
    long long created_at = now_s;
    std::error_code ec;
    if (fs::exists(path, ec)) {
        try {
            std::ifstream existing(path);
            if (existing.is_open()) {
                json prev = json::parse(existing, nullptr, /*allow_exceptions=*/false);
                if (prev.is_object() && prev.contains("created_at") &&
                    prev["created_at"].is_number_integer())
                    created_at = prev["created_at"].get<long long>();
            }
        } catch (...) {
            // Treat unreadable existing file as a fresh save.
        }
    }
    j["created_at"]     = created_at;
    j["last_opened_at"] = now_s;

    // Extract first user message for preview.
    std::string preview;
    for (auto& msg : history.messages()) {
        if (msg.role == MessageRole::user) {
            preview = msg.content.substr(0, 120);
            if (msg.content.size() > 120) preview += "...";
            break;
        }
    }
    j["first_user_message"] = preview;

    // Merge top-level extras (e.g. "metrics", "title") in last so saved
    // metadata can co-exist with future session-level fields without schema
    // bumps. Caller supplies `title` here when the tab title is already known.
    if (extras.is_object()) {
        for (auto it = extras.begin(); it != extras.end(); ++it)
            j[it.key()] = it.value();
    }

    // Default title from the first user message if none was supplied.
    if (!j.contains("title") || (j["title"].is_string() &&
                                  j["title"].get<std::string>().empty()))
        j["title"] = derive_title_from_preview(preview);

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

namespace {

// S6.17 Task G (ADR-0008) one-version shim: rewrite legacy `{name:"search"}`
// tool calls in saved-session JSON into the matching per-mode tool name
// (search_text / search_regex / ...). Drops the now-meaningless `mode` arg.
// `tool_calls[].arguments` is a JSON string at the session-file layer (it
// rides as the raw string the API returned), so we parse it, edit, dump.
// Removed next milestone.
void rewrite_legacy_search_calls(json& messages)
{
    if (!messages.is_array()) return;
    for (auto& msg : messages) {
        if (!msg.is_object()) continue;
        auto it = msg.find("tool_calls");
        if (it == msg.end() || !it->is_array()) continue;
        for (auto& tc : *it) {
            if (!tc.is_object()) continue;
            auto fn = tc.find("function");
            if (fn == tc.end() || !fn->is_object()) continue;
            auto name_it = fn->find("name");
            if (name_it == fn->end() || !name_it->is_string()) continue;
            if (name_it->get<std::string>() != "search") continue;

            // Parse the arguments string; default to text mode when missing.
            std::string mode = "text";
            json args = json::object();
            auto args_it = fn->find("arguments");
            if (args_it != fn->end() && args_it->is_string()) {
                try {
                    args = json::parse(args_it->get<std::string>());
                } catch (...) {
                    args = json::object();
                }
            }
            if (args.is_object()) {
                auto m = args.find("mode");
                if (m != args.end() && m->is_string())
                    mode = m->get<std::string>();
                args.erase("mode");
            }

            std::string new_name = "search_" + mode;
            (*fn)["name"]      = new_name;
            (*fn)["arguments"] = args.dump();
        }
    }
}

}  // namespace

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

    rewrite_legacy_search_calls(j["messages"]);

    auto history = ConversationHistory::from_json(j["messages"]);
    spdlog::info("SessionManager: loaded session '{}' ({} messages)",
                 id, history.size());
    return history;
}

json SessionManager::load_full(const std::string& id) const
{
    auto path = sessions_dir_ / (id + ".json");

    if (!fs::exists(path))
        throw std::runtime_error("Session not found: " + id);

    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot read session file: " + path.string());

    json j = json::parse(f);
    if (!j.is_object() || !j.contains("messages"))
        throw std::runtime_error("Invalid session file (no messages): " + id);

    rewrite_legacy_search_calls(j["messages"]);
    return j;
}

// -- List ---------------------------------------------------------------------

SessionInfo SessionManager::read_session_info(const fs::path& path)
{
    SessionInfo info;
    info.file_path = path.string();
    info.id = path.stem().string();
    info.timestamp = info.id;  // ID is the timestamp-prefixed string

    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    info.size_bytes = ec ? 0 : static_cast<long long>(sz);

    // S5.I -- a session can also have a sibling `<id>/` directory holding the
    // S5.F pre-compaction history archive snapshots. Sum its bytes so the
    // Manage Sessions dialog reports total on-disk footprint, not just the
    // live JSON file.
    auto archive_dir = path.parent_path() / info.id;
    if (fs::is_directory(archive_dir, ec)) {
        std::error_code walk_ec;
        for (auto& entry : fs::recursive_directory_iterator(archive_dir,
                fs::directory_options::skip_permission_denied, walk_ec)) {
            if (walk_ec) break;
            if (!entry.is_regular_file(walk_ec)) continue;
            auto fsz = fs::file_size(entry.path(), walk_ec);
            if (!walk_ec) info.size_bytes += static_cast<long long>(fsz);
        }
    }

    // For legacy fields default to file mtime so the cleanup heuristic still
    // works on pre-S5.I session files. fs::last_write_time clock is not
    // necessarily system_clock, but the conversion below is good enough for
    // a "days old" cutoff -- we only care about coarse ordering.
    long long file_mtime = 0;
    auto ft = fs::last_write_time(path, ec);
    if (!ec) {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - decltype(ft)::clock::now() + std::chrono::system_clock::now());
        file_mtime = std::chrono::duration_cast<std::chrono::seconds>(
            sctp.time_since_epoch()).count();
    }

    try {
        std::ifstream f(path);
        if (!f.is_open()) return info;

        json j = json::parse(f);
        info.message_count     = j.value("message_count",       0);
        info.estimated_tokens  = j.value("estimated_tokens",    0);
        info.first_user_message = j.value("first_user_message", std::string{});
        info.title             = j.value("title",               std::string{});
        info.created_at        = j.value("created_at",          file_mtime);
        info.last_opened_at    = j.value("last_opened_at",      file_mtime);
    } catch (const json::exception& e) {
        spdlog::warn("SessionManager: cannot read session info from {}: {}",
                     path.string(), e.what());
    }

    // Fallback title: derived from first user message preview.
    if (info.title.empty())
        info.title = derive_title_from_preview(info.first_user_message);

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

    // Sort by last_opened_at desc (newest open first). Fall back to id when
    // timestamps tie so the order is deterministic across calls.
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionInfo& a, const SessionInfo& b) {
                  if (a.last_opened_at != b.last_opened_at)
                      return a.last_opened_at > b.last_opened_at;
                  return a.id > b.id;
              });

    spdlog::trace("SessionManager: listed {} sessions", sessions.size());
    return sessions;
}

// -- Remove -------------------------------------------------------------------

bool SessionManager::remove(const std::string& id)
{
    auto path         = sessions_dir_ / (id + ".json");
    auto archive_dir  = sessions_dir_ / id;
    auto activity_log = sessions_dir_ / (id + ".activity.jsonl");

    std::error_code ec;
    bool removed_file = fs::remove(path, ec);

    // S5.I -- a session can also have a sibling `<id>/` directory holding the
    // S5.F pre-compaction history archives. Remove it too in the same
    // operation; partial cleanup (file removed but archives left behind) would
    // make the cleanup heuristic believe the session is gone while the
    // workspace silently keeps growing.
    std::error_code ec_dir;
    auto removed_archives = fs::remove_all(archive_dir, ec_dir);  // 0 on missing

    // Activity-log sidecar (opt-in, may not exist). Drop it together with
    // the session so a delete-and-recreate cycle doesn't replay stale
    // activity into the new run.
    std::error_code ec_act;
    bool removed_activity = fs::remove(activity_log, ec_act);

    if (removed_file)
        spdlog::info("SessionManager: removed session '{}' (+{} archive file(s){})",
                     id, static_cast<unsigned long long>(removed_archives),
                     removed_activity ? ", +activity log" : "");
    else if (removed_archives > 0 || removed_activity)
        spdlog::info("SessionManager: removed orphaned files for '{}' "
                     "({} archive file(s){})", id,
                     static_cast<unsigned long long>(removed_archives),
                     removed_activity ? ", +activity log" : "");

    return removed_file || removed_archives > 0 || removed_activity;
}

// -- Patch helpers ------------------------------------------------------------

bool SessionManager::patch_field(const std::string& id,
                                 const std::string& key,
                                 const nlohmann::json& value)
{
    auto path = sessions_dir_ / (id + ".json");
    if (!fs::exists(path))
        return false;

    json j;
    {
        std::ifstream f(path);
        if (!f.is_open()) {
            spdlog::warn("SessionManager: cannot read {} for patch", path.string());
            return false;
        }
        try {
            j = json::parse(f);
        } catch (const json::exception& e) {
            spdlog::warn("SessionManager: parse error patching {}: {}",
                         path.string(), e.what());
            return false;
        }
    }
    j[key] = value;

    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("SessionManager: cannot write {} for patch", path.string());
        return false;
    }
    f << j.dump(2) << '\n';
    return true;
}

bool SessionManager::bump_last_opened_at(const std::string& id)
{
    return patch_field(id, "last_opened_at", now_epoch_seconds());
}

bool SessionManager::rename(const std::string& id, const std::string& new_title)
{
    return patch_field(id, "title", new_title);
}

// -- Cleanup ------------------------------------------------------------------

std::vector<std::string> SessionManager::cleanup_candidates(
    int keep_last_count,
    int delete_after_days,
    const std::unordered_set<std::string>& currently_open) const
{
    std::vector<SessionInfo> all = list();

    // Drop currently-open sessions from the candidate pool entirely.
    all.erase(std::remove_if(all.begin(), all.end(),
        [&](const SessionInfo& s) {
            return currently_open.count(s.id) > 0;
        }), all.end());

    // `all` is sorted last_opened_at desc. Skip the top keep_last_count rows;
    // those are explicitly protected by the "small workspace" clause.
    int skip = keep_last_count > 0 ? keep_last_count : 0;
    long long cutoff = now_epoch_seconds()
        - static_cast<long long>(delete_after_days) * 24LL * 3600LL;

    std::vector<std::string> out;
    for (size_t i = static_cast<size_t>(skip); i < all.size(); ++i) {
        if (all[i].last_opened_at <= cutoff)
            out.push_back(all[i].id);
    }
    // Oldest-first so the dialog can show them in deletion order.
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace locus
