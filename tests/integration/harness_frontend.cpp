#include "harness_frontend.h"

#include <spdlog/spdlog.h>

namespace locus::integration {

HarnessFrontend::HarnessFrontend() = default;

void HarnessFrontend::reset_turn_state()
{
    {
        std::lock_guard lock(mutex_);
        tokens_.clear();
        tool_calls_.clear();
        tool_results_.clear();
        errors_.clear();
    }
    {
        std::lock_guard lock(turn_mutex_);
        turn_complete_ = false;
    }
}

bool HarnessFrontend::wait_for_turn(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(turn_mutex_);
    return turn_cv_.wait_for(lock, timeout, [this] { return turn_complete_; });
}

void HarnessFrontend::queue_ask_user_response(std::string answer)
{
    std::lock_guard lock(mutex_);
    ask_user_queue_.push_back(std::move(answer));
}

std::string HarnessFrontend::tokens() const
{
    std::lock_guard lock(mutex_);
    return tokens_;
}

std::vector<ObservedToolCall> HarnessFrontend::tool_calls() const
{
    std::lock_guard lock(mutex_);
    return tool_calls_;
}

std::vector<ObservedToolResult> HarnessFrontend::tool_results() const
{
    std::lock_guard lock(mutex_);
    return tool_results_;
}

std::vector<std::string> HarnessFrontend::errors() const
{
    std::lock_guard lock(mutex_);
    return errors_;
}

void HarnessFrontend::on_turn_start() {}

void HarnessFrontend::on_token(std::string_view token)
{
    std::lock_guard lock(mutex_);
    tokens_.append(token);
}

void HarnessFrontend::on_reasoning_token(std::string_view /*token*/) {}

void HarnessFrontend::on_tool_call_pending(const ToolCall& call,
                                           const std::string& preview)
{
    // Record the observation first.
    {
        std::lock_guard lock(mutex_);
        tool_calls_.push_back({call.id, call.tool_name, call.args, preview});
    }

    if (!core_) {
        spdlog::error("HarnessFrontend: tool_call_pending fired but core not attached");
        return;
    }

    // ask_user gets a scripted answer (if any) via ToolDecision::modify. We must
    // preserve the original question because the tool reads args["question"]
    // when formatting its result.
    if (call.tool_name == "ask_user") {
        std::string answer;
        bool has_answer = false;
        {
            std::lock_guard lock(mutex_);
            if (!ask_user_queue_.empty()) {
                answer = std::move(ask_user_queue_.front());
                ask_user_queue_.pop_front();
                has_answer = true;
            }
        }
        if (has_answer) {
            nlohmann::json modified = call.args;
            modified["response"] = answer;
            core_->tool_decision(call.id, ToolDecision::modify, modified);
            return;
        }
        // No scripted answer: approve without a response. The tool will return
        // "[User approved without providing a response]".
        core_->tool_decision(call.id, ToolDecision::approve, {});
        return;
    }

    // Everything else: auto-approve.
    core_->tool_decision(call.id, ToolDecision::approve, {});
}

void HarnessFrontend::on_tool_result(const std::string& call_id,
                                     const std::string& display)
{
    std::lock_guard lock(mutex_);
    tool_results_.push_back({call_id, display});
}

void HarnessFrontend::on_turn_complete()
{
    std::lock_guard lock(turn_mutex_);
    turn_complete_ = true;
    turn_cv_.notify_all();
}

void HarnessFrontend::on_context_meter(int /*used*/, int /*limit*/) {}
void HarnessFrontend::on_compaction_needed(int /*used*/, int /*limit*/) {}
void HarnessFrontend::on_session_reset() {}

void HarnessFrontend::on_error(const std::string& message)
{
    std::lock_guard lock(mutex_);
    errors_.push_back(message);
}

void HarnessFrontend::on_embedding_progress(int done, int total)
{
    embedding_done_.store(done);
    embedding_total_.store(total);
}

void HarnessFrontend::on_indexing_progress(int done, int total)
{
    indexing_done_.store(done);
    indexing_total_.store(total);
}

void HarnessFrontend::on_activity(const ActivityEvent& /*event*/) {}

} // namespace locus::integration
