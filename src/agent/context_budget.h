#pragma once

#include "../core/frontend_registry.h"

#include <algorithm>

namespace locus {

// Tracks token usage vs. the context window limit and decides when the
// frontends should be asked to compact. Owns the "last server-reported
// usage" and "previous-turn total" counters used for delta reporting.
class ContextBudget {
public:
    static constexpr double compaction_threshold = 0.80;

    ContextBudget(int context_limit, FrontendRegistry& frontends);

    int  limit() const { return limit_; }

    // S5.D -- minimum response headroom. The agent loop refuses to dispatch
    // an LLM round when prompt_estimate + reserve_ > limit_. Zero means no
    // reserve is enforced (the pre-S5.D behaviour).
    void set_reserve(int r)       { reserve_ = (r > 0 ? r : 0); }
    int  reserve()          const { return reserve_; }

    // Effective prompt capacity: context window minus the reserved headroom.
    // check_overflow thresholds are computed against this value so the user
    // sees compaction warnings relative to usable space, not the raw window.
    int  effective_limit()  const { return std::max(1, limit_ - reserve_); }

    // Best-effort current token count. Uses the last server-reported total
    // when non-zero, otherwise the supplied heuristic fallback.
    int  current(int fallback_estimate) const;

    // Latest server-reported total_tokens from a completion usage payload.
    void set_server_total(int total);
    int  server_total() const { return server_total_; }

    // S4.V Task 8 -- prompt / completion split from the same usage payload.
    // Stored alongside `server_total_` so frontends can render "ctx: T/L
    // [p:P g:G]" without re-deriving the split. 0 means "not yet reported"
    // (e.g. the first context-meter broadcast happens at session open
    // before any LLM round).
    void set_server_split(int prompt, int completion);
    int  last_prompt_tokens()     const { return last_prompt_tokens_; }
    int  last_completion_tokens() const { return last_completion_tokens_; }

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
    int               reserve_               = 0;
    FrontendRegistry& frontends_;
    int               server_total_          = 0;
    int               prev_turn_total_       = 0;
    int               last_prompt_tokens_    = 0;
    int               last_completion_tokens_= 0;
};

} // namespace locus
