#include "llm/token_counter.h"

namespace locus {

int TokenCounter::estimate(const std::string& text)
{
    return static_cast<int>((text.size() + 3) / 4);
}

int TokenCounter::estimate(const std::vector<ChatMessage>& messages)
{
    int total = 0;
    for (auto& m : messages) {
        // Role overhead: ~4 tokens per message framing.
        total += 4;
        total += estimate(m.content);
        for (auto& tc : m.tool_calls) {
            total += estimate(tc.name);
            total += estimate(tc.arguments);
        }
    }
    return total;
}

} // namespace locus
