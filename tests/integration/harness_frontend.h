#pragma once

#include "activity_event.h"
#include "frontend.h"
#include "tool.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace locus::integration {

// Snapshot of a single tool-call pending event we observed mid-turn.
struct ObservedToolCall {
    std::string    id;
    std::string    tool_name;
    nlohmann::json args;
    std::string    preview;
};

// Snapshot of a tool-result event we observed.
struct ObservedToolResult {
    std::string call_id;
    std::string display;
};

// Test frontend that drives the approval gate synchronously, records every
// callback, and signals turn completion via a condvar so the fixture can do
// prompt / wait-for-turn / assert flows.
//
// All callbacks fire on the agent thread (and embedding/indexing callbacks on
// their respective worker threads). Internal state is mutex-guarded.
class HarnessFrontend final : public IFrontend {
public:
    HarnessFrontend();
    ~HarnessFrontend() override = default;

    // Must be called exactly once after AgentCore is constructed, before the
    // first turn is submitted. Lets the frontend call back into the core to
    // approve/reject tool calls.
    void attach_core(ILocusCore* core) { core_ = core; }

    // Clear per-turn recorded state. Call this before each prompt.
    void reset_turn_state();

    // Wait until on_turn_complete fires. Returns false on timeout.
    bool wait_for_turn(std::chrono::milliseconds timeout);

    // Queue a scripted response for the next ask_user call. Multiple queued
    // responses are consumed in FIFO order.
    void queue_ask_user_response(std::string answer);

    // Snapshots (thread-safe copies).
    std::string                      tokens() const;
    std::vector<ObservedToolCall>    tool_calls() const;
    std::vector<ObservedToolResult>  tool_results() const;
    std::vector<std::string>         errors() const;
    int  indexing_done()   const { return indexing_done_.load(); }
    int  indexing_total()  const { return indexing_total_.load(); }
    int  embedding_done()  const { return embedding_done_.load(); }
    int  embedding_total() const { return embedding_total_.load(); }

    // -- IFrontend ------------------------------------------------------------

    void on_turn_start() override;
    void on_token(std::string_view token) override;
    void on_reasoning_token(std::string_view token) override;
    void on_tool_call_pending(const ToolCall& call, const std::string& preview) override;
    void on_tool_result(const std::string& call_id, const std::string& display) override;
    void on_turn_complete() override;
    void on_context_meter(int used_tokens, int limit) override;
    void on_compaction_needed(int used_tokens, int limit) override;
    void on_session_reset() override;
    void on_error(const std::string& message) override;
    void on_embedding_progress(int done, int total) override;
    void on_indexing_progress(int done, int total) override;
    void on_activity(const ActivityEvent& event) override;

private:
    ILocusCore* core_ = nullptr;

    mutable std::mutex mutex_;
    std::string                     tokens_;
    std::vector<ObservedToolCall>   tool_calls_;
    std::vector<ObservedToolResult> tool_results_;
    std::vector<std::string>        errors_;
    std::deque<std::string>         ask_user_queue_;

    std::atomic<int> indexing_done_{0};
    std::atomic<int> indexing_total_{0};
    std::atomic<int> embedding_done_{0};
    std::atomic<int> embedding_total_{0};

    std::mutex              turn_mutex_;
    std::condition_variable turn_cv_;
    bool                    turn_complete_ = false;
};

} // namespace locus::integration
