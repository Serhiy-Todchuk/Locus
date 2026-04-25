#include "llm/llm_router.h"

#include <stdexcept>
#include <utility>

namespace locus {

LlmRouter::LlmRouter(std::unique_ptr<ILLMClient> strong,
                     std::unique_ptr<ILLMClient> weak)
    : strong_(std::move(strong))
    , weak_(std::move(weak))
{
    if (!strong_)
        throw std::invalid_argument("LlmRouter requires a non-null strong client");
}

ILLMClient& LlmRouter::select(Role role)
{
    if (role == Role::Weak && weak_)
        return *weak_;
    return *strong_;
}

} // namespace locus
