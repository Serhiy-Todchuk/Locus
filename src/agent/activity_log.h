#pragma once

#include "activity_event.h"
#include "frontend_registry.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace locus {

// Thread-safe activity ring buffer. Assigns monotonic ids, broadcasts every
// event through the supplied FrontendRegistry, and keeps the last N for
// late-joining frontends that need to catch up via get_since().
class ActivityLog {
public:
    explicit ActivityLog(FrontendRegistry& frontends, size_t max_size = 1000);

    void emit(ActivityKind kind,
              std::string summary,
              std::string detail = {},
              std::optional<int> tokens_in = std::nullopt,
              std::optional<int> tokens_out = std::nullopt,
              std::optional<int> tokens_delta = std::nullopt);

    void emit_index_event(const std::string& summary,
                          const std::string& detail = {});

    std::vector<ActivityEvent> get_since(uint64_t since_id) const;

private:
    FrontendRegistry&          frontends_;
    size_t                     max_size_;
    mutable std::mutex         mutex_;
    std::vector<ActivityEvent> buffer_;
    uint64_t                   next_id_ = 1;
};

} // namespace locus
