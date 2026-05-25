#pragma once

#include "../core/activity_event.h"
#include "../core/frontend_registry.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace locus {

// Thread-safe activity ring buffer. Assigns monotonic ids, broadcasts every
// event through the supplied FrontendRegistry, and keeps the last N for
// late-joining frontends that need to catch up via get_since().
//
// Optional persistence (off by default): set_persist_path(path) routes every
// subsequent emit() to an append-only JSONL sidecar at `path`. The ring buffer
// behaviour is unchanged -- the file is the durability surface, the buffer is
// still the in-memory hot view. `replay_from(path)` reads the sidecar back,
// rebuilds the in-memory buffer, and broadcasts the events as on_activity()
// callbacks so a fresh activity panel paints the restored history identically.
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

    // Sidecar persistence (off by default). Pass an empty path to disable.
    // Called from AgentCore::save_session right after the session id is known
    // and from AgentCore::load_session before replay. Idempotent.
    void set_persist_path(const std::filesystem::path& path);

    // Reads the JSONL sidecar at `path`, rebuilds the in-memory ring buffer
    // from its contents (dropping the oldest entries when count > max_size),
    // and re-broadcasts each event via on_activity(). Resets next_id_ so
    // future emits continue from the last replayed id + 1. Missing or
    // unreadable file is treated as "no history to replay", not an error.
    void replay_from(const std::filesystem::path& path);

    // Clears in-memory buffer + resets the id counter. Used when a workspace
    // switches active session (load_session into a fresh session) so the
    // activity panel doesn't show events bleeding across the boundary.
    void clear();

private:
    void append_to_sidecar(const ActivityEvent& ev);  // mutex_ NOT held

    FrontendRegistry&          frontends_;
    size_t                     max_size_;
    mutable std::mutex         mutex_;
    std::vector<ActivityEvent> buffer_;
    uint64_t                   next_id_ = 1;
    std::filesystem::path      persist_path_;        // empty = disabled
};

} // namespace locus
