#pragma once

#include "llm_client.h"

#include <string>
#include <vector>

namespace locus {

// Manages the conversation message history. Provides serialization, token
// estimation, and compaction helpers.
class ConversationHistory {
public:
    void add(ChatMessage msg);
    void clear();

    const std::vector<ChatMessage>& messages() const { return messages_; }
    size_t size() const { return messages_.size(); }
    bool   empty() const { return messages_.empty(); }

    // Estimated token count for the entire history.
    int estimate_tokens() const;

    // -- Compaction -------------------------------------------------------

    // Strategy B: strip content from all tool-result messages (keep the
    // message frame so the LLM knows a tool was called, but drop the bulk).
    void drop_tool_results();

    // Strategy C: remove the oldest n user+assistant turn pairs.
    // The system message (index 0) is never removed.
    void drop_oldest_turns(int n);

    // -- Serialization ----------------------------------------------------

    nlohmann::json to_json() const;
    static ConversationHistory from_json(const nlohmann::json& j);

private:
    std::vector<ChatMessage> messages_;
};

} // namespace locus
