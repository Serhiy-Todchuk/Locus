#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace locus {

// ---- Types ------------------------------------------------------------------

// Matches OpenAI chat completion message roles.
enum class MessageRole { system, user, assistant, tool };

inline const char* to_string(MessageRole r)
{
    switch (r) {
    case MessageRole::system:    return "system";
    case MessageRole::user:      return "user";
    case MessageRole::assistant: return "assistant";
    case MessageRole::tool:      return "tool";
    }
    return "user";
}

inline MessageRole role_from_string(const std::string& s)
{
    if (s == "system")    return MessageRole::system;
    if (s == "assistant") return MessageRole::assistant;
    if (s == "tool")      return MessageRole::tool;
    return MessageRole::user;
}

// A single tool call requested by the LLM.
struct ToolCallRequest {
    std::string id;           // e.g. "call_abc123"
    std::string name;         // tool/function name
    std::string arguments;    // raw JSON string of arguments
};

// A chat message (user, assistant, system, or tool result).
struct ChatMessage {
    MessageRole role = MessageRole::user;
    std::string content;

    // For assistant messages that contain tool calls:
    std::vector<ToolCallRequest> tool_calls;

    // For tool-result messages:
    std::string tool_call_id;

    nlohmann::json to_json() const;
    static ChatMessage from_json(const nlohmann::json& j);
};

// ---- Config -----------------------------------------------------------------

struct LLMConfig {
    std::string base_url      = "http://127.0.0.1:1234";
    std::string model         = "";          // empty = use server default
    double      temperature   = 0.7;
    int         max_tokens    = 2048;
    int         context_limit = 8192;        // total context window size
    int         timeout_ms    = 60000;       // stream stall timeout: abort if no bytes flow for this long. Not a total-request cap — long reasoning streams are fine.
};

// ---- Model info (from /v1/models) -------------------------------------------

struct ModelInfo {
    std::string id;              // e.g. "gemma-3-4b-instruct"
    int         context_length = 0;  // max context window (0 = unknown)
};

// ---- Completion usage (from response) ---------------------------------------

struct CompletionUsage {
    int prompt_tokens     = 0;
    int completion_tokens = 0;
    int total_tokens      = 0;
    int reasoning_tokens  = 0;   // subset of completion_tokens (reasoning models)
};

// ---- Streaming callbacks ----------------------------------------------------

// Called for each text token as it arrives.
using OnToken = std::function<void(const std::string& token)>;

// Called when one or more tool calls are fully accumulated.
using OnToolCalls = std::function<void(const std::vector<ToolCallRequest>& calls)>;

// Called when the response is complete (after [DONE]).
using OnComplete = std::function<void()>;

// Called on error (network, parse, timeout).
using OnError = std::function<void(const std::string& error)>;

// Called with token usage from the server (if available in the response).
using OnUsage = std::function<void(const CompletionUsage& usage)>;

struct StreamCallbacks {
    OnToken     on_token;
    OnToken     on_reasoning_token;   // chain-of-thought tokens (LM Studio's
                                      // reasoning_content); empty = ignored
    OnToolCalls on_tool_calls;
    OnComplete  on_complete;
    OnError     on_error;
    OnUsage     on_usage;
};

// ---- Tool schema (for function calling) ------------------------------------

struct ToolSchema {
    std::string     name;
    std::string     description;
    nlohmann::json  parameters;   // JSON Schema object
};

// ---- Interface --------------------------------------------------------------

// Abstract LLM client. Implementations handle transport and streaming.
class ILLMClient {
public:
    virtual ~ILLMClient() = default;

    // Send a chat completion request with streaming. Blocks until the
    // response is fully consumed or an error occurs. Callbacks fire on
    // the calling thread.
    virtual void stream_completion(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolSchema>&  tools,
        const StreamCallbacks&          callbacks) = 0;

    // Query the server for loaded model info (id, context_length).
    // Returns the first loaded model, or empty ModelInfo on failure.
    virtual ModelInfo query_model_info() = 0;
};

// Factory: creates a LMStudioClient (OpenAI-compatible) with the given config.
std::unique_ptr<ILLMClient> create_llm_client(LLMConfig config);

} // namespace locus
