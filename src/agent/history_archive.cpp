#include "history_archive.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace locus {

namespace fs = std::filesystem;

HistoryArchive::HistoryArchive(std::filesystem::path sessions_dir)
    : sessions_dir_(std::move(sessions_dir))
{
    std::error_code ec;
    fs::create_directories(sessions_dir_, ec);
    if (ec)
        spdlog::warn("HistoryArchive: cannot create archive root {}: {}",
                     sessions_dir_.string(), ec.message());
}

fs::path HistoryArchive::session_dir(const std::string& session_id) const
{
    return sessions_dir_ / session_id;
}

int HistoryArchive::counter_from_filename(const std::string& filename)
{
    // history.before-compact-<N>.json
    static const std::regex re(R"(^history\.before-compact-(\d+)\.json$)");
    std::smatch m;
    if (!std::regex_match(filename, m, re)) return 0;
    try { return std::stoi(m[1].str()); }
    catch (...) { return 0; }
}

std::string HistoryArchive::footnote_relative_path(const std::string& session_id,
                                                    int counter)
{
    std::ostringstream o;
    o << ".locus/sessions/" << session_id
      << "/history.before-compact-" << counter << ".json";
    return o.str();
}

int HistoryArchive::highest_counter(const std::string& session_id) const
{
    auto files = list(session_id);
    if (files.empty()) return 0;
    // list() returns ascending by counter, so the back is the largest.
    return counter_from_filename(files.back().filename().string());
}

std::vector<fs::path> HistoryArchive::list(const std::string& session_id) const
{
    std::vector<fs::path> out;
    auto dir = session_dir(session_id);
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return out;

    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().string();
        if (counter_from_filename(name) > 0)
            out.push_back(entry.path());
    }

    std::sort(out.begin(), out.end(),
              [](const fs::path& a, const fs::path& b) {
                  return counter_from_filename(a.filename().string())
                       < counter_from_filename(b.filename().string());
              });
    return out;
}

fs::path HistoryArchive::snapshot(const std::string& session_id,
                                   const ConversationHistory& history)
{
    auto dir = session_dir(session_id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        spdlog::warn("HistoryArchive: cannot create session dir {}: {}",
                     dir.string(), ec.message());
        return {};
    }

    auto existing = list(session_id);
    int next_n = 1;
    if (!existing.empty()) {
        int last = counter_from_filename(existing.back().filename().string());
        next_n = last + 1;
    }

    std::ostringstream name;
    name << "history.before-compact-" << next_n << ".json";
    auto path = dir / name.str();

    nlohmann::json j;
    j["session_id"]       = session_id;
    j["counter"]          = next_n;
    j["message_count"]    = static_cast<int>(history.size());
    j["estimated_tokens"] = history.estimate_tokens();
    j["messages"]         = history.to_json();

    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        spdlog::warn("HistoryArchive: cannot open {} for write", path.string());
        return {};
    }
    f << j.dump(2) << '\n';
    spdlog::info("HistoryArchive: snapshot '{}' #{} ({} messages, ~{} tokens) -> {}",
                 session_id, next_n, history.size(),
                 history.estimate_tokens(), path.string());
    return path;
}

void HistoryArchive::gc(const std::string& session_id, int keep_count)
{
    if (keep_count <= 0) return;
    auto files = list(session_id);
    if (static_cast<int>(files.size()) <= keep_count) return;

    int to_remove = static_cast<int>(files.size()) - keep_count;
    for (int i = 0; i < to_remove; ++i) {
        std::error_code ec;
        fs::remove(files[static_cast<std::size_t>(i)], ec);
        if (ec) {
            spdlog::warn("HistoryArchive: gc remove {} failed: {}",
                         files[static_cast<std::size_t>(i)].string(),
                         ec.message());
        } else {
            spdlog::trace("HistoryArchive: gc removed {}",
                          files[static_cast<std::size_t>(i)].string());
        }
    }
}

} // namespace locus
