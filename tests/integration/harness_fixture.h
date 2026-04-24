#pragma once

#include "harness_frontend.h"

#include "agent/agent_core.h"
#include "agent/system_prompt.h"
#include "llm_client.h"
#include "tool_registry.h"
#include "workspace.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace locus::integration {

// Mirror stderr log output at trace level so test runs are observable live in
// the console. Must be called before the harness is first accessed (logger is
// init'd once, on first `IntegrationHarness::shared()`).
void set_console_logging(bool enabled);
bool is_console_logging();

struct PromptResult {
    std::string                     tokens;
    std::vector<ObservedToolCall>   tool_calls;
    std::vector<ObservedToolResult> tool_results;
    std::vector<std::string>        errors;
    bool                            timed_out = false;

    bool tool_called(std::string_view name) const;
    const ObservedToolCall* find_tool_call(std::string_view name) const;
    const ObservedToolResult* find_result_for(std::string_view tool_name) const;
};

// The shared integration harness. Heavy — opens the repo as a workspace,
// connects to LM Studio, starts an agent thread. Lazy-constructed on first
// access and kept alive for the life of the process.
class IntegrationHarness {
public:
    // Returns the shared instance. First call verifies LM Studio is reachable
    // and throws std::runtime_error on failure. Subsequent calls return the
    // already-built harness.
    static IntegrationHarness& shared();

    // Explicit teardown (called once from a Catch2 event listener at session
    // end). Safe to call more than once.
    static void shutdown_shared();

    ~IntegrationHarness();

    IntegrationHarness(const IntegrationHarness&) = delete;
    IntegrationHarness& operator=(const IntegrationHarness&) = delete;

    // Workspace root (absolute, canonical).
    const std::filesystem::path& workspace_root() const { return workspace_root_; }

    // Scratch directory guaranteed empty + present at harness startup, removed
    // at shutdown. Tests can create/delete files here freely.
    const std::filesystem::path& tmp_dir() const { return tmp_dir_; }

    Workspace&       workspace() { return *workspace_; }
    AgentCore&       agent()     { return *agent_; }
    HarnessFrontend& frontend()  { return *frontend_; }
    ILLMClient&      llm()       { return *llm_; }
    const std::string& model_id() const { return model_id_; }

    // Send a user message, wait for turn completion, return captured events.
    // Default timeout is generous because local LLMs can be slow.
    PromptResult prompt(const std::string& text,
                        std::chrono::milliseconds timeout = std::chrono::minutes(5));

    // Wait until the watcher pump has seen recent changes and the indexer has
    // caught up. Call after test-side file mutations and before prompts that
    // depend on the index seeing them.
    void wait_for_index_idle(std::chrono::milliseconds timeout = std::chrono::seconds(20));

    // Wait until the embedding worker queue has drained. No-op if semantic
    // search is disabled (embedding_worker() == nullptr).
    void wait_for_embedding_idle(std::chrono::milliseconds timeout = std::chrono::minutes(2));

private:
    IntegrationHarness();

    std::filesystem::path workspace_root_;
    std::filesystem::path tmp_dir_;

    LLMConfig                    llm_config_;
    std::string                  model_id_;
    std::unique_ptr<ILLMClient>  llm_;
    std::unique_ptr<Workspace>   workspace_;
    std::unique_ptr<ToolRegistry> tools_;
    std::unique_ptr<AgentCore>   agent_;
    std::unique_ptr<HarnessFrontend> frontend_;
};

// Convenience — same as IntegrationHarness::shared().
inline IntegrationHarness& harness() { return IntegrationHarness::shared(); }

} // namespace locus::integration
