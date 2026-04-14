#include "conversation.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace locus {

void ConversationHistory::add(ChatMessage msg)
{
    messages_.push_back(std::move(msg));
}

void ConversationHistory::clear()
{
    messages_.clear();
}

int ConversationHistory::estimate_tokens() const
{
    return ILLMClient::estimate_tokens(messages_);
}

// -- Compaction ---------------------------------------------------------------

void ConversationHistory::drop_tool_results()
{
    int dropped = 0;
    for (auto& m : messages_) {
        if (m.role == MessageRole::tool && !m.content.empty()) {
            m.content = "[tool result removed for context space]";
            ++dropped;
        }
    }
    spdlog::info("Compaction B: replaced {} tool result(s) with placeholder", dropped);
}

void ConversationHistory::drop_oldest_turns(int n)
{
    if (n <= 0 || messages_.empty()) return;

    // Find the first non-system message.
    size_t start = 0;
    if (!messages_.empty() && messages_[0].role == MessageRole::system)
        start = 1;

    // Count turn pairs (user + assistant + any tool messages between them).
    int turns_dropped = 0;
    size_t erase_end = start;

    for (size_t i = start; i < messages_.size() && turns_dropped < n; ++i) {
        erase_end = i + 1;
        // A "turn" ends after an assistant message that has no tool_calls,
        // or after the tool results following an assistant message with tool_calls.
        if (messages_[i].role == MessageRole::assistant) {
            if (messages_[i].tool_calls.empty()) {
                ++turns_dropped;
            } else {
                // Skip past the tool result messages that follow.
                while (erase_end < messages_.size() &&
                       messages_[erase_end].role == MessageRole::tool) {
                    ++erase_end;
                }
                ++turns_dropped;
                i = erase_end - 1;  // -1 because the for loop increments
            }
        }
    }

    if (erase_end > start) {
        messages_.erase(messages_.begin() + static_cast<ptrdiff_t>(start),
                        messages_.begin() + static_cast<ptrdiff_t>(erase_end));
        spdlog::info("Compaction C: dropped {} oldest turn(s), {} messages remain",
                     turns_dropped, messages_.size());
    }
}

// -- Serialization ------------------------------------------------------------

nlohmann::json ConversationHistory::to_json() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (auto& m : messages_)
        arr.push_back(m.to_json());
    return arr;
}

ConversationHistory ConversationHistory::from_json(const nlohmann::json& j)
{
    ConversationHistory h;
    if (j.is_array()) {
        for (auto& item : j)
            h.messages_.push_back(ChatMessage::from_json(item));
    }
    return h;
}

} // namespace locus
