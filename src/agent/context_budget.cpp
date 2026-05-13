#include "context_budget.h"

#include "../core/frontend.h"

#include <spdlog/spdlog.h>

namespace locus {

ContextBudget::ContextBudget(int context_limit, FrontendRegistry& frontends)
    : limit_(context_limit)
    , frontends_(frontends)
{}

int ContextBudget::current(int fallback_estimate) const
{
    return server_total_ > 0 ? server_total_ : fallback_estimate;
}

void ContextBudget::set_server_total(int total)
{
    server_total_ = total;
}

void ContextBudget::set_server_split(int prompt, int completion)
{
    last_prompt_tokens_     = prompt;
    last_completion_tokens_ = completion;
}

void ContextBudget::mark_turn_total(int total)
{
    prev_turn_total_ = total;
}

void ContextBudget::reset()
{
    server_total_           = 0;
    prev_turn_total_        = 0;
    last_prompt_tokens_     = 0;
    last_completion_tokens_ = 0;
}

bool ContextBudget::check_overflow(int used)
{
    double ratio = limit_ > 0 ? static_cast<double>(used) / limit_ : 0.0;

    if (ratio >= 1.0) {
        spdlog::warn("ContextBudget: context FULL ({}/{} tokens, {:.0f}%)",
                     used, limit_, ratio * 100);
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_compaction_needed(used, limit_);
        });
        return true;
    }

    if (ratio >= compaction_threshold) {
        spdlog::warn("ContextBudget: context at {:.0f}% ({}/{} tokens)",
                     ratio * 100, used, limit_);
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_compaction_needed(used, limit_);
        });
    }

    return false;
}

} // namespace locus
