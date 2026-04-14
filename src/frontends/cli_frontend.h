#pragma once

#include "../frontend.h"

#include <string>

namespace locus {

// CLI frontend: renders the agent conversation in a terminal.
// All callbacks fire on the agent thread (the thread that called send_message).
// Tool approval reads from stdin, blocking the agent until the user responds.
class CliFrontend : public IFrontend {
public:
    // core pointer is needed so on_tool_call_pending can call tool_decision().
    explicit CliFrontend(ILocusCore& core);

    void on_token(std::string_view token) override;
    void on_tool_call_pending(const ToolCall& call,
                              const std::string& preview) override;
    void on_tool_result(const std::string& call_id,
                        const std::string& display) override;
    void on_message_complete() override;
    void on_context_meter(int used_tokens, int limit) override;
    void on_compaction_needed(int used_tokens, int limit) override;
    void on_error(const std::string& message) override;

    // Last context meter values, for displaying in the input prompt.
    int last_used() const { return last_used_; }
    int last_limit() const { return last_limit_; }

private:
    // Read a single-char decision from stdin. Returns 'y', 'n', or 'e'.
    static char read_decision();

    // Read modified args JSON from stdin (for the 'edit' flow).
    static nlohmann::json read_modified_args(const nlohmann::json& original);

    ILocusCore& core_;
    int last_used_  = 0;
    int last_limit_ = 0;
};

} // namespace locus
