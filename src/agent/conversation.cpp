#include "conversation.h"

#include "llm/token_counter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <sstream>

namespace locus {

// Copy/move: the owner_thread_id_ atomic is non-copyable; treat thread ownership
// as a runtime property of the live instance and reset it on copy/move. The
// assignment variants assert the destination's owner so a cross-thread replace
// during an active turn still trips the fence.
ConversationHistory::ConversationHistory(const ConversationHistory& other)
    : messages_(other.messages_)
{}

ConversationHistory::ConversationHistory(ConversationHistory&& other) noexcept
    : messages_(std::move(other.messages_))
{}

ConversationHistory& ConversationHistory::operator=(const ConversationHistory& other)
{
    if (this == &other) return *this;
    assert_owner_thread("operator=(copy)");
    messages_ = other.messages_;
    return *this;
}

ConversationHistory& ConversationHistory::operator=(ConversationHistory&& other) noexcept
{
    if (this == &other) return *this;
    assert_owner_thread("operator=(move)");
    messages_ = std::move(other.messages_);
    return *this;
}

void ConversationHistory::add(ChatMessage msg)
{
    assert_owner_thread("add");
    messages_.push_back(std::move(msg));
}

void ConversationHistory::clear()
{
    assert_owner_thread("clear");
    messages_.clear();
}

void ConversationHistory::replace_system_prompt(std::string content)
{
    assert_owner_thread("replace_system_prompt");
    if (!messages_.empty() && messages_.front().role == MessageRole::system) {
        messages_.front().content = std::move(content);
    } else {
        messages_.insert(messages_.begin(),
                         ChatMessage{MessageRole::system, std::move(content)});
    }
}

int ConversationHistory::estimate_tokens() const
{
    return TokenCounter::estimate(messages_);
}

// -- Compaction ---------------------------------------------------------------

void ConversationHistory::drop_tool_results()
{
    assert_owner_thread("drop_tool_results");
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
    assert_owner_thread("drop_oldest_turns");
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

// -- Thread ownership (S3.I) -------------------------------------------------

void ConversationHistory::set_owner_thread(std::thread::id id)
{
    owner_thread_id_.store(id, std::memory_order_relaxed);
}

void ConversationHistory::clear_owner_thread()
{
    owner_thread_id_.store(std::thread::id{}, std::memory_order_relaxed);
}

void ConversationHistory::assert_owner_thread(const char* op) const
{
    auto owner = owner_thread_id_.load(std::memory_order_relaxed);
    if (owner == std::thread::id{}) return;              // no owner set -> any thread may write
    auto caller = std::this_thread::get_id();
    if (owner == caller) return;                         // on owner thread -> ok

    std::ostringstream os_owner, os_caller;
    os_owner  << owner;
    os_caller << caller;
    spdlog::error("ConversationHistory::{}: non-owner thread write (owner={}, caller={})",
                  op, os_owner.str(), os_caller.str());
    assert(false && "ConversationHistory accessed from non-owner thread");
}

} // namespace locus
