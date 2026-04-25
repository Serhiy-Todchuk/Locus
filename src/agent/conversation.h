#pragma once

#include "llm/llm_client.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace locus {

// Manages the conversation message history. Provides serialization, token
// estimation, and compaction helpers.
class ConversationHistory {
public:
    ConversationHistory() = default;
    ConversationHistory(const ConversationHistory& other);
    ConversationHistory(ConversationHistory&& other) noexcept;
    ConversationHistory& operator=(const ConversationHistory& other);
    ConversationHistory& operator=(ConversationHistory&& other) noexcept;

    void add(ChatMessage msg);
    void clear();

    // Overwrite the leading system message's content (or insert one if the
    // history is empty / does not start with a system role). Used when the
    // attached-context section of the system prompt is added or removed.
    void replace_system_prompt(std::string content);

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

    // -- Thread ownership (S3.I) ------------------------------------------
    // When an owner thread is set, every mutating call asserts it is on that
    // thread (spdlog::error + debug abort). AgentCore sets the owner around
    // each in-turn call to process_message; between turns the owner is
    // cleared so inter-turn operations (CLI REPL /reset etc.) run on any
    // thread without tripping the fence. Tests may set/clear the owner
    // directly when simulating the agent thread.
    void set_owner_thread(std::thread::id id);
    void clear_owner_thread();
    void assert_owner_thread(const char* op) const;

private:
    std::vector<ChatMessage>     messages_;
    std::atomic<std::thread::id> owner_thread_id_{};
};

// RAII helper: set ConversationHistory's owner thread to the caller for the
// duration of the scope, then restore (clear). Used by AgentCore to gate each
// process_message() run to the agent thread.
class ConversationOwnerScope {
public:
    explicit ConversationOwnerScope(ConversationHistory& h)
        : history_(h)
    {
        history_.set_owner_thread(std::this_thread::get_id());
    }
    ~ConversationOwnerScope() { history_.clear_owner_thread(); }

    ConversationOwnerScope(const ConversationOwnerScope&) = delete;
    ConversationOwnerScope& operator=(const ConversationOwnerScope&) = delete;

private:
    ConversationHistory& history_;
};

} // namespace locus
