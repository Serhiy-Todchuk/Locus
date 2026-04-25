#include "checkpoint_store.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace locus {

namespace {

// FNV-1a 64-bit. Cheap, dependency-free, plenty for "did the bytes change"
// identification — not used for any security purpose.
uint64_t fnv1a64(const char* data, size_t n)
{
    constexpr uint64_t k_offset = 1469598103934665603ull;
    constexpr uint64_t k_prime  = 1099511628211ull;
    uint64_t h = k_offset;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= k_prime;
    }
    return h;
}

std::string to_hex16(uint64_t v)
{
    std::ostringstream os;
    os << std::hex << std::setw(16) << std::setfill('0') << v;
    return os.str();
}

std::string iso_timestamp_now()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return os.str();
}

// Read the full bytes of a file. Returns false if it can't be opened.
bool slurp(const fs::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool spit(const fs::path& p, const std::string& bytes)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

} // namespace

CheckpointStore::CheckpointStore(fs::path checkpoints_dir)
    : root_(std::move(checkpoints_dir))
{
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec)
        spdlog::warn("CheckpointStore: cannot create root '{}': {}",
                     root_.string(), ec.message());
}

fs::path CheckpointStore::session_dir(const std::string& session_id) const
{
    return root_ / session_id;
}

fs::path CheckpointStore::turn_dir(const std::string& session_id, int turn_id) const
{
    return session_dir(session_id) / std::to_string(turn_id);
}

fs::path CheckpointStore::manifest_path(const std::string& session_id, int turn_id) const
{
    return turn_dir(session_id, turn_id) / "checkpoint.json";
}

std::string CheckpointStore::make_relative(const fs::path& workspace_root,
                                           const fs::path& abs_path)
{
    std::error_code ec;
    auto rel = fs::relative(abs_path, workspace_root, ec);
    if (ec || rel.empty())
        return abs_path.generic_string();
    return rel.generic_string();
}

std::string CheckpointStore::hash_file(const fs::path& p)
{
    std::string buf;
    if (!slurp(p, buf)) return {};
    return to_hex16(fnv1a64(buf.data(), buf.size()));
}

bool CheckpointStore::snapshot_before(const std::string& session_id,
                                      int turn_id,
                                      const fs::path& workspace_root,
                                      const fs::path& abs_path,
                                      const std::string& action)
{
    if (session_id.empty()) {
        spdlog::trace("CheckpointStore: no session_id, skipping snapshot");
        return false;
    }

    std::lock_guard lock(io_mutex_);

    std::string rel = make_relative(workspace_root, abs_path);

    // Skip if we already snapshotted this path in this turn.
    auto turn = read_turn(session_id, turn_id);
    if (turn) {
        for (const auto& e : turn->entries) {
            if (e.path == rel) {
                spdlog::trace("CheckpointStore: already snapshotted '{}' in turn {}",
                              rel, turn_id);
                return true;
            }
        }
    }

    CheckpointEntry entry;
    entry.path     = rel;
    entry.action   = action;
    entry.existed  = false;
    entry.skipped  = false;
    entry.size_bytes = 0;

    std::error_code ec;
    bool exists = fs::exists(abs_path, ec);
    if (exists && !fs::is_regular_file(abs_path, ec))
        exists = false;

    if (exists) {
        entry.existed = true;
        entry.size_bytes = fs::file_size(abs_path, ec);
        if (ec) entry.size_bytes = 0;

        if (entry.size_bytes > max_file_size_bytes_) {
            entry.skipped = true;
            spdlog::warn("CheckpointStore: skipping snapshot of '{}' "
                         "({} bytes > {} byte limit) — undo via git instead",
                         rel, entry.size_bytes, max_file_size_bytes_);
        } else {
            std::string bytes;
            if (!slurp(abs_path, bytes)) {
                spdlog::warn("CheckpointStore: cannot read '{}' for snapshot",
                             abs_path.string());
                return false;
            }
            entry.prev_hash = to_hex16(fnv1a64(bytes.data(), bytes.size()));

            fs::path snap = turn_dir(session_id, turn_id) / "files" / rel;
            if (!spit(snap, bytes)) {
                spdlog::warn("CheckpointStore: cannot write snapshot '{}'",
                             snap.string());
                return false;
            }
        }
    }

    // Append the new entry to the manifest (read-modify-write — IO is small).
    nlohmann::json manifest;
    auto mpath = manifest_path(session_id, turn_id);
    if (fs::exists(mpath)) {
        std::ifstream f(mpath);
        try {
            f >> manifest;
        } catch (const std::exception& ex) {
            spdlog::warn("CheckpointStore: corrupt manifest '{}', reinitialising: {}",
                         mpath.string(), ex.what());
            manifest = nlohmann::json::object();
        }
    }
    if (!manifest.contains("session_id")) manifest["session_id"] = session_id;
    if (!manifest.contains("turn_id"))    manifest["turn_id"]    = turn_id;
    if (!manifest.contains("created_at")) manifest["created_at"] = iso_timestamp_now();
    if (!manifest.contains("entries"))    manifest["entries"]    = nlohmann::json::array();

    nlohmann::json je;
    je["path"]       = entry.path;
    je["action"]     = entry.action;
    je["prev_hash"]  = entry.prev_hash;
    je["existed"]    = entry.existed;
    je["skipped"]    = entry.skipped;
    je["size_bytes"] = entry.size_bytes;
    manifest["entries"].push_back(je);

    fs::create_directories(mpath.parent_path(), ec);
    std::ofstream out(mpath, std::ios::trunc);
    if (!out.is_open()) {
        spdlog::warn("CheckpointStore: cannot write manifest '{}'", mpath.string());
        return false;
    }
    out << manifest.dump(2) << '\n';

    spdlog::trace("CheckpointStore: snapshot '{}' action={} existed={} skipped={} size={}",
                  rel, action, entry.existed, entry.skipped, entry.size_bytes);
    return true;
}

std::optional<TurnInfo> CheckpointStore::read_turn(const std::string& session_id,
                                                   int turn_id) const
{
    auto mpath = manifest_path(session_id, turn_id);
    if (!fs::exists(mpath)) return std::nullopt;

    std::ifstream f(mpath);
    if (!f.is_open()) return std::nullopt;

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& ex) {
        spdlog::warn("CheckpointStore: cannot parse manifest '{}': {}",
                     mpath.string(), ex.what());
        return std::nullopt;
    }

    TurnInfo info;
    info.session_id = j.value("session_id", session_id);
    info.turn_id    = j.value("turn_id", turn_id);
    info.created_at = j.value("created_at", "");

    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& je : j["entries"]) {
            CheckpointEntry e;
            e.path       = je.value("path", "");
            e.action     = je.value("action", "");
            e.prev_hash  = je.value("prev_hash", "");
            e.existed    = je.value("existed", false);
            e.skipped    = je.value("skipped", false);
            e.size_bytes = je.value("size_bytes", 0ull);
            info.entries.push_back(std::move(e));
        }
    }
    return info;
}

int CheckpointStore::entry_count(const std::string& session_id, int turn_id) const
{
    auto info = read_turn(session_id, turn_id);
    if (!info) return 0;
    return static_cast<int>(info->entries.size());
}

RestoreResult CheckpointStore::restore_turn(const std::string& session_id,
                                            int turn_id,
                                            const fs::path& workspace_root) const
{
    RestoreResult result;
    auto info = read_turn(session_id, turn_id);
    if (!info) {
        result.errors.push_back("No checkpoint for turn " + std::to_string(turn_id));
        return result;
    }

    fs::path files_root = turn_dir(session_id, turn_id) / "files";

    for (const auto& e : info->entries) {
        fs::path target = workspace_root / e.path;

        if (e.skipped) {
            ++result.skipped;
            result.errors.push_back("Skipped (>limit) at snapshot time: " + e.path);
            continue;
        }

        if (!e.existed) {
            // File was created during the turn — undo by deleting it.
            std::error_code ec;
            if (fs::exists(target, ec)) {
                fs::remove(target, ec);
                if (ec) {
                    ++result.skipped;
                    result.errors.push_back("Cannot delete '" + e.path + "': " + ec.message());
                } else {
                    ++result.deleted;
                }
            }
            continue;
        }

        fs::path snap = files_root / e.path;
        if (!fs::exists(snap)) {
            ++result.skipped;
            result.errors.push_back("Missing snapshot file: " + e.path);
            continue;
        }

        std::string bytes;
        if (!slurp(snap, bytes)) {
            ++result.skipped;
            result.errors.push_back("Cannot read snapshot for '" + e.path + "'");
            continue;
        }

        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (!spit(target, bytes)) {
            ++result.skipped;
            result.errors.push_back("Cannot write '" + e.path + "'");
            continue;
        }
        ++result.restored;
    }

    spdlog::info("CheckpointStore: restored turn {} of session '{}': "
                 "{} restored, {} deleted, {} skipped",
                 turn_id, session_id, result.restored, result.deleted, result.skipped);
    return result;
}

bool CheckpointStore::drop_turn(const std::string& session_id, int turn_id)
{
    auto dir = turn_dir(session_id, turn_id);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;
    fs::remove_all(dir, ec);
    if (ec)
        spdlog::warn("CheckpointStore: drop_turn failed for {}: {}",
                     dir.string(), ec.message());
    return !ec;
}

bool CheckpointStore::drop_session(const std::string& session_id)
{
    auto dir = session_dir(session_id);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;
    fs::remove_all(dir, ec);
    if (ec)
        spdlog::warn("CheckpointStore: drop_session failed for {}: {}",
                     dir.string(), ec.message());
    return !ec;
}

int CheckpointStore::gc_session(const std::string& session_id)
{
    if (max_turns_per_session_ <= 0) return 0;

    auto dir = session_dir(session_id);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;

    std::vector<int> turn_ids;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;
        try {
            int n = std::stoi(entry.path().filename().string());
            turn_ids.push_back(n);
        } catch (...) {
            // ignore non-numeric subdirs
        }
    }
    if (static_cast<int>(turn_ids.size()) <= max_turns_per_session_) return 0;

    std::sort(turn_ids.begin(), turn_ids.end());
    int to_drop = static_cast<int>(turn_ids.size()) - max_turns_per_session_;
    int pruned = 0;
    for (int i = 0; i < to_drop; ++i) {
        if (drop_turn(session_id, turn_ids[i])) ++pruned;
    }
    if (pruned > 0)
        spdlog::info("CheckpointStore: GC pruned {} turn(s) from session '{}' "
                     "(kept {})",
                     pruned, session_id, max_turns_per_session_);
    return pruned;
}

std::vector<TurnInfo> CheckpointStore::list_turns(const std::string& session_id) const
{
    std::vector<TurnInfo> out;
    auto dir = session_dir(session_id);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;

    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;
        int n = 0;
        try { n = std::stoi(entry.path().filename().string()); }
        catch (...) { continue; }
        if (auto info = read_turn(session_id, n)) out.push_back(std::move(*info));
    }
    std::sort(out.begin(), out.end(),
              [](const TurnInfo& a, const TurnInfo& b) { return a.turn_id < b.turn_id; });
    return out;
}

} // namespace locus
