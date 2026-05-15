#pragma once

#include "core/activity_event.h"
#include "core/frontend.h"
#include "tools/tool.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
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

// S4.D plan-mode event captures.
struct ObservedPlanStepAdvance {
    std::string      plan_id;
    int              step_idx = 0;     // 0-based
    PlanStep::Status status = PlanStep::Status::pending;
    std::string      notes;
};

struct ObservedPlanCompletion {
    std::string plan_id;
    bool        success = false;
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

    // S4.D plan event captures (thread-safe copies).
    std::optional<Plan>                   last_plan_proposed() const;
    std::vector<ObservedPlanStepAdvance>  plan_step_advances() const;
    std::vector<ObservedPlanCompletion>   plan_completions()   const;
    std::vector<AgentMode>                mode_changes()       const;

    // Re-arm the turn-complete latch without clearing recorded state. Use
    // before triggering a follow-on turn (e.g. approve_plan auto-resume)
    // so wait_for_turn blocks for the *next* turn rather than returning
    // immediately because the previous one already completed.
    void arm_next_turn();

    // -- IFrontend ------------------------------------------------------------

    void on_turn_start() override;
    void on_token(std::string_view token) override;
    void on_reasoning_token(std::string_view token) override;
    void on_tool_call_pending(const ToolCall& call, const std::string& preview, bool needs_approval,
                              const std::vector<std::string>& safety_warnings) override;
    void on_tool_result(const std::string& call_id, const std::string& display, bool success) override;
    void on_turn_complete() override;
    void on_context_meter(int used_tokens, int limit,
                          int prompt_tokens, int completion_tokens,
                          int reserve_tokens = 0) override;
    void on_compaction_needed(int used_tokens, int limit) override;
    void on_session_reset() override;
    void on_error(const std::string& message) override;
    void on_embedding_progress(int done, int total) override;
    void on_indexing_progress(int done, int total) override;
    void on_activity(const ActivityEvent& event) override;
    // S4.D
    void on_mode_changed(AgentMode mode) override;
    void on_plan_proposed(const Plan& plan) override;
    void on_plan_step_advanced(const std::string& plan_id, int step_idx,
                                PlanStep::Status status,
                                const std::string& notes) override;
    void on_plan_completed(const std::string& plan_id, bool success) override;

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

    // S4.D plan event records (mutex_-guarded).
    std::optional<Plan>                  last_plan_proposed_;
    std::vector<ObservedPlanStepAdvance> plan_step_advances_;
    std::vector<ObservedPlanCompletion>  plan_completions_;
    std::vector<AgentMode>               mode_changes_;
};

} // namespace locus::integration
