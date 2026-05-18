#include "activity_log.h"

#include "../core/frontend.h"

#include <cctype>
#include <chrono>
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

} // namespace locus
