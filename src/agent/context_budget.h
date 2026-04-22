#pragma once

#include "frontend_registry.h"

namespace locus {

// Tracks token usage vs. the context window limit and decides when the
// frontends should be asked to compact. Owns the "last server-reported
// usage" and "previous-turn total" counters used for delta reporting.
class ContextBudget {
public:
    static constexpr double compaction_threshold = 0.80;

    ContextBudget(int context_limit, FrontendRegistry& frontends);

    int  limit() const { return limit_; }

    // Best-effort current token count. Uses the last server-reported total
    // when non-zero, otherwise the supplied heuristic fallback.
    int  current(int fallback_estimate) const;

    // Latest server-reported total_tokens from a completion usage payload.
    void set_server_total(int total);
    int  server_total() const { return server_total_; }

    // Remember the running total after the last completed LLM step; used to
    // compute `delta` in the next activity event.
    void mark_turn_total(int total);
    int  prev_turn_total() const { return prev_turn_total_; }

    // Reset counters after reset_conversation / load_session.
    void reset();

    // Returns true when `used` has hit or exceeded the hard limit (the
    // caller should stop the current turn). Fires on_compaction_needed
    // through the frontend registry on both soft- and hard-limit crossings.
    bool check_overflow(int used);

private:
    int               limit_;
    FrontendRegistry& frontends_;
    int               server_total_ = 0;
    int               prev_turn_total_ = 0;
};

} // namespace locus
