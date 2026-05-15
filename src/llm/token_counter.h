#pragma once

#include "llm/llm_client.h"

#include <string>
#include <vector>

namespace locus {

// Heuristic token counter (~4 chars = 1 token, +4 framing per message).
// Cheap, deterministic, good enough for budget tracking with local models.
// S4.F will introduce real BPE / HF tokenisation behind this same surface.
class TokenCounter {
public:
    static int estimate(const std::string& text);
    static int estimate(const std::vector<ChatMessage>& messages);

    // S5.D -- per-message estimate: the same value estimate(messages) assigns
    // per message (4 framing tokens + estimate(content) + tool-call bodies).
    static int estimate_message(const ChatMessage& msg);
};

} // namespace locus
