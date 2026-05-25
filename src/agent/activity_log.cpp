#include "activity_log.h"

#include "../core/frontend.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <chrono>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace locus {

namespace {

// Returns the substring of `s` up to (but not including) the first ASCII
// digit. Used as a category key for index_event coalescing -- every emitter
// in the codebase formats its summary as "<words> <count> ..." (e.g.
// "Indexed 5 files", "Embedding progress: 50/100"), so two events share a
// category iff they share this prefix. "Indexed 1 file" and "Indexed 12
// files" both reduce to "Indexed ", so they coalesce; "Indexed ..." vs
// "Embedding progress: ..." don't, so they stay as separate rows.
//
// Defined here (in the .cpp) so it's not a public surface -- the rule is
// internal to ActivityLog and may change.
std::string category_prefix(const std::string& s)
{
    size_t i = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i])))
        ++i;
    return s.substr(0, i);
}

ActivityKind kind_from_string(const std::string& s)
{
    if (s == "system_prompt")      return ActivityKind::system_prompt;
    if (s == "tool_manifest")      return ActivityKind::tool_manifest;
    if (s == "user_message")       return ActivityKind::user_message;
    if (s == "llm_response")       return ActivityKind::llm_response;
    if (s == "tool_call")          return ActivityKind::tool_call;
    if (s == "tool_result")        return ActivityKind::tool_result;
    if (s == "index_event")        return ActivityKind::index_event;
    if (s == "memory_added")       return ActivityKind::memory_added;
    if (s == "memory_searched")    return ActivityKind::memory_searched;
    if (s == "memory_deleted")     return ActivityKind::memory_deleted;
    if (s == "compaction")         return ActivityKind::compaction;
    if (s == "truncation_blocked") return ActivityKind::truncation_blocked;
    if (s == "quality_correction") return ActivityKind::quality_correction;
    if (s == "error")              return ActivityKind::error;
    return ActivityKind::warning;
}

} // namespace

ActivityLog::ActivityLog(FrontendRegistry& frontends, size_t max_size)
    : frontends_(frontends)
    , max_size_(max_size)
{}

void ActivityLog::emit(ActivityKind kind,
                       std::string summary,
                       std::string detail,
                       std::optional<int> tokens_in,
                       std::optional<int> tokens_out,
                       std::optional<int> tokens_delta)
{
    ActivityEvent ev;
    bool was_coalesced = false;
    {
        std::lock_guard lock(mutex_);

        // Coalesce consecutive index_event rows that share a category prefix.
        // Indexer / EmbeddingWorker can fire many of these in a burst (one per
        // file-watcher batch, one per N embedded chunks); without coalescing
        // they push everything else out of the visible window. The latest
        // event's summary + detail + timestamp replace the previous row;
        // id stays the same so frontends can update the existing widget.
        const bool can_coalesce =
            kind == ActivityKind::index_event &&
            !buffer_.empty() &&
            buffer_.back().kind == ActivityKind::index_event &&
            category_prefix(buffer_.back().summary) == category_prefix(summary);

        if (can_coalesce) {
            buffer_.back().summary   = std::move(summary);
            buffer_.back().detail    = std::move(detail);
            buffer_.back().timestamp = std::chrono::system_clock::now();
            // tokens_in/out/delta intentionally untouched -- index events
            // don't carry token data, so the previous values (all nullopt)
            // remain correct.
            ev = buffer_.back();
            was_coalesced = true;
        } else {
            ev.id = next_id_++;
            ev.timestamp = std::chrono::system_clock::now();
            ev.kind = kind;
            ev.summary = std::move(summary);
            ev.detail = std::move(detail);
            ev.tokens_in = tokens_in;
            ev.tokens_out = tokens_out;
            ev.tokens_delta = tokens_delta;

            buffer_.push_back(ev);
            if (buffer_.size() > max_size_) {
                buffer_.erase(buffer_.begin(),
                              buffer_.begin() +
                                  (buffer_.size() - max_size_));
            }
        }
    }

    // Sidecar JSONL is append-only -- we never rewrite a coalesced line, we
    // just append the new (latest) state. The replay reader keeps the LAST
    // entry per id, so this stays consistent without holding the mutex
    // through file I/O.
    append_to_sidecar(ev);

    if (was_coalesced)
        frontends_.broadcast([&](IFrontend& fe) { fe.on_activity_updated(ev); });
    else
        frontends_.broadcast([&](IFrontend& fe) { fe.on_activity(ev); });
}

void ActivityLog::emit_index_event(const std::string& summary,
                                   const std::string& detail)
{
    emit(ActivityKind::index_event, summary, detail);
}

std::vector<ActivityEvent> ActivityLog::get_since(uint64_t since_id) const
{
    std::lock_guard lock(mutex_);
    std::vector<ActivityEvent> out;
    out.reserve(buffer_.size());
    for (auto& ev : buffer_)
        if (ev.id > since_id)
            out.push_back(ev);
    return out;
}

// -- Sidecar persistence -----------------------------------------------------

void ActivityLog::set_persist_path(const std::filesystem::path& path)
{
    std::lock_guard lock(mutex_);
    persist_path_ = path;
}

void ActivityLog::clear()
{
    std::lock_guard lock(mutex_);
    buffer_.clear();
    next_id_ = 1;
}

void ActivityLog::append_to_sidecar(const ActivityEvent& ev)
{
    std::filesystem::path path;
    {
        std::lock_guard lock(mutex_);
        if (persist_path_.empty()) return;
        path = persist_path_;
    }

    nlohmann::json j;
    j["id"]      = ev.id;
    j["ts"]      = std::chrono::duration_cast<std::chrono::seconds>(
                       ev.timestamp.time_since_epoch()).count();
    j["kind"]    = to_string(ev.kind);
    j["summary"] = ev.summary;
    if (!ev.detail.empty()) j["detail"] = ev.detail;
    if (ev.tokens_in.has_value())    j["tin"]    = *ev.tokens_in;
    if (ev.tokens_out.has_value())   j["tout"]   = *ev.tokens_out;
    if (ev.tokens_delta.has_value()) j["tdelta"] = *ev.tokens_delta;

    try {
        std::ofstream out(path, std::ios::app | std::ios::binary);
        if (!out.is_open()) return;
        out << j.dump() << '\n';
    } catch (const std::exception& e) {
        spdlog::warn("ActivityLog: sidecar append failed at {}: {}",
                     path.string(), e.what());
    }
}

void ActivityLog::replay_from(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return;

    // Read all lines into events, keep last-wins per id (coalesced events
    // append a fresh line each time -- the latest line is the truth).
    std::vector<ActivityEvent> ordered;
    std::unordered_map<uint64_t, size_t> by_id;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        nlohmann::json j;
        try { j = nlohmann::json::parse(line); }
        catch (const std::exception& e) {
            spdlog::warn("ActivityLog: sidecar parse failure at {}: {}",
                         path.string(), e.what());
            continue;
        }
        if (!j.is_object() || !j.contains("id")) continue;

        ActivityEvent ev;
        ev.id      = j.value("id", 0ULL);
        long long ts = j.value("ts", 0LL);
        ev.timestamp = std::chrono::system_clock::time_point(
                           std::chrono::seconds(ts));
        ev.kind    = kind_from_string(j.value("kind", std::string{"warning"}));
        ev.summary = j.value("summary", std::string{});
        ev.detail  = j.value("detail",  std::string{});
        if (j.contains("tin")    && j["tin"].is_number_integer())
            ev.tokens_in    = j["tin"].get<int>();
        if (j.contains("tout")   && j["tout"].is_number_integer())
            ev.tokens_out   = j["tout"].get<int>();
        if (j.contains("tdelta") && j["tdelta"].is_number_integer())
            ev.tokens_delta = j["tdelta"].get<int>();

        auto it = by_id.find(ev.id);
        if (it == by_id.end()) {
            by_id[ev.id] = ordered.size();
            ordered.push_back(std::move(ev));
        } else {
            ordered[it->second] = std::move(ev);  // last-wins
        }
    }

    if (ordered.empty()) return;

    // Cap to the configured ring size, keeping the most-recent entries.
    if (ordered.size() > max_size_) {
        ordered.erase(ordered.begin(),
                      ordered.begin() +
                          (ordered.size() - max_size_));
    }

    uint64_t high = 0;
    for (auto& ev : ordered) high = std::max(high, ev.id);

    {
        std::lock_guard lock(mutex_);
        buffer_ = ordered;
        next_id_ = high + 1;
    }

    // Re-broadcast so any frontend attached at load time paints the restored
    // activity rows. Done outside the lock to avoid reentrancy on listeners
    // that might call get_since() / emit() in response.
    for (const auto& ev : ordered) {
        frontends_.broadcast([&](IFrontend& fe) { fe.on_activity(ev); });
    }
}

} // namespace locus
