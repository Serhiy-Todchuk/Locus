#include "llm/token_counter.h"

namespace locus {

int TokenCounter::estimate(const std::string& text)
{
    return static_cast<int>((text.size() + 3) / 4);
}

int TokenCounter::estimate(const std::vector<ChatMessage>& messages)
{
    int total = 0;
    for (auto& m : messages)
        total += estimate_message(m);
    return total;
}

int TokenCounter::estimate_message(const ChatMessage& msg)
{
    // Role overhead: ~4 tokens per message framing.
    int total = 4 + estimate(msg.content);
    for (auto& tc : msg.tool_calls) {
        total += estimate(tc.name);
        total += estimate(tc.arguments);
    }
    return total;
}

} // namespace locus
