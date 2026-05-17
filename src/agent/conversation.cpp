#include "conversation.h"

#include "llm/token_counter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <sstream>
#include <unordered_set>

namespace locus {

// Copy/move: the owner_thread_id_ atomic is non-copyable; treat thread ownership
// as a runtime property of the live instance and reset it on copy/move. The
// assignment variants assert the destination's owner so a cross-thread replace
// during an active turn still trips the fence.
ConversationHistory::ConversationHistory(const ConversationHistory& other)
    : messages_(other.messages_)
    , next_history_id_(other.next_history_id_)
{}

ConversationHistory::ConversationHistory(ConversationHistory&& other) noexcept
    : messages_(std::move(other.messages_))
    , next_history_id_(other.next_history_id_)
{}

ConversationHistory& ConversationHistory::operator=(const ConversationHistory& other)
{
    if (this == &other) return *this;
    assert_owner_thread("operator=(copy)");
    messages_ = other.messages_;
    next_history_id_ = other.next_history_id_;
    return *this;
}

ConversationHistory& ConversationHistory::operator=(ConversationHistory&& other) noexcept
{
    if (this == &other) return *this;
    assert_owner_thread("operator=(move)");
    messages_ = std::move(other.messages_);
    next_history_id_ = other.next_history_id_;
    return *this;
}

void ConversationHistory::add(ChatMessage msg)
{
    assert_owner_thread("add");
    // S5.D -- cache the per-message token estimate so callers can display it
    // without re-computing on every render. Computed here (once) rather than
    // in TokenCounter::estimate so the value is stable for the message's lifetime.
    if (msg.token_estimate == 0)
        msg.token_estimate = TokenCounter::estimate_message(msg);
    // S5.G -- assign a stable history_id only if the caller hasn't set one
    // (e.g. test code constructing a message with a hand-picked id). Counter
    // is monotonic and never re-uses after delete.
    if (msg.history_id == 0)
        msg.history_id = ++next_history_id_;
    else
        next_history_id_ = std::max(next_history_id_, msg.history_id);
    messages_.push_back(std::move(msg));
}

void ConversationHistory::clear()
{
    assert_owner_thread("clear");
    messages_.clear();
    next_history_id_ = 0;
}

bool ConversationHistory::delete_by_id(int history_id)
{
    assert_owner_thread("delete_by_id");
    if (history_id <= 0) return false;
    auto it = std::find_if(messages_.begin(), messages_.end(),
                           [history_id](const ChatMessage& m) {
                               return m.history_id == history_id;
                           });
    if (it == messages_.end()) return false;
    // System prompt (leading entry) is owned by AgentCore + must stay byte-stable
    // for S4.F prefix-cache reuse. Refuse the delete here so the contract holds
    // regardless of which caller asks.
    if (it->role == MessageRole::system) {
        spdlog::warn("ConversationHistory::delete_by_id: refusing to delete system message (id={})",
                     history_id);
        return false;
    }
    messages_.erase(it);
    return true;
}

std::vector<int> ConversationHistory::delete_tool_call_pair(int history_id)
{
    assert_owner_thread("delete_tool_call_pair");
    if (history_id <= 0) return {};

    auto it = std::find_if(messages_.begin(), messages_.end(),
                           [history_id](const ChatMessage& m) {
                               return m.history_id == history_id;
                           });
    if (it == messages_.end()) return {};
    if (it->role != MessageRole::assistant || it->tool_calls.empty()) return {};

    std::unordered_set<std::string> call_ids;
    for (const auto& tc : it->tool_calls) {
        if (!tc.id.empty()) call_ids.insert(tc.id);
    }

    std::vector<int> deleted;
    deleted.reserve(1 + call_ids.size());
    deleted.push_back(history_id);
    for (const auto& m : messages_) {
        if (m.role == MessageRole::tool &&
            call_ids.count(m.tool_call_id) > 0) {
            deleted.push_back(m.history_id);
        }
    }

    messages_.erase(
        std::remove_if(messages_.begin(), messages_.end(),
                       [&](const ChatMessage& m) {
                           if (m.history_id == history_id) return true;
                           return m.role == MessageRole::tool &&
                                  call_ids.count(m.tool_call_id) > 0;
                       }),
        messages_.end());

    return deleted;
}

void ConversationHistory::replace_system_prompt(std::string content)
{
    assert_owner_thread("replace_system_prompt");
    if (!messages_.empty() && messages_.front().role == MessageRole::system) {
        messages_.front().content = std::move(content);
    } else {
        ChatMessage sys{MessageRole::system, std::move(content)};
        if (sys.history_id == 0)
            sys.history_id = ++next_history_id_;
        messages_.insert(messages_.begin(), std::move(sys));
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
        for (auto& item : j) {
            auto msg = ChatMessage::from_json(item);
            // S5.D -- back-fill estimate for messages loaded from pre-S5.D sessions.
            if (msg.token_estimate == 0)
                msg.token_estimate = TokenCounter::estimate_message(msg);
            // S5.G -- re-walk and assign fresh history_ids. The wire format
            // deliberately doesn't carry the id (LLM doesn't need it), and
            // re-numbering on load means save -> close -> open survives even
            // when an earlier session had deletes that left gaps.
            msg.history_id = ++h.next_history_id_;
            h.messages_.push_back(std::move(msg));
        }
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
