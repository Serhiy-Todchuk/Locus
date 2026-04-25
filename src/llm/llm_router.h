#pragma once

#include "llm/llm_client.h"

#include <memory>

namespace locus {

// Routes LLM calls to one of two backends: a strong (slower, smarter)
// client and a weak (fast, cheap) client. Compaction summarisation,
// scratch tool dispatch, and cheap classification go to weak; the main
// agent loop goes to strong. S4.Q wires this; today the router simply
// returns the strong client for every Role (weak optional).
class LlmRouter {
public:
    enum class Role { Strong, Weak };

    // weak may be null -- in which case Role::Weak falls back to strong.
    LlmRouter(std::unique_ptr<ILLMClient> strong,
              std::unique_ptr<ILLMClient> weak = nullptr);

    ILLMClient& select(Role role);

    // Direct access for code that always wants the main client. Once
    // S4.Q lands every caller should pick a Role explicitly.
    ILLMClient& strong() { return *strong_; }

private:
    std::unique_ptr<ILLMClient> strong_;
    std::unique_ptr<ILLMClient> weak_;
};

} // namespace locus
