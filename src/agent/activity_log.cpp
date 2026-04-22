#include "activity_log.h"

#include "frontend.h"

#include <chrono>
#include <utility>

namespace locus {

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
    {
        std::lock_guard lock(mutex_);
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
