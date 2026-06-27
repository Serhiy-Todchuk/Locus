#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
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

    // S6.10 Task C -- assistant chain-of-thought reasoning, captured from the
    // stream decoder's `on_reasoning` channel (LM Studio splits this into the
    // OpenAI delta's `reasoning_content` field; Qwen-XML / Claude-XML decoders
    // extract <think>...</think> / <thinking>...</thinking> bodies and route
    // them here too). Persisted to disk via to_json; stripped from past-turn
    // assistant messages when assembling the next request payload (see
    // strip_past_thinking in WorkspaceConfig and AgentLoop's payload-prep).
    // Empty for non-assistant messages and for assistant messages from a
    // model that has no reasoning channel.
    std::string reasoning_content;

    // For assistant messages that contain tool calls:
    std::vector<ToolCallRequest> tool_calls;

    // For tool-result messages:
    std::string tool_call_id;

    // S5.D -- heuristic token estimate populated by TokenCounter::estimate_message()
    // when the message is added to ConversationHistory. 0 for messages loaded from
    // pre-S5.D session files (estimate_message() re-computes on add/from_json).
    int token_estimate = 0;

    // S5.G -- stable per-message id assigned by ConversationHistory::add(). Used
    // by the chat panel's per-message delete (locus://delete-message/<id>) and
    // by IFrontend::on_history_message_added. NOT serialized via to_json (the
    // LLM wire format must stay clean); ConversationHistory::from_json re-walks
    // and re-assigns monotonic ids so saved-and-loaded sessions get fresh ones.
    int history_id = 0;

    nlohmann::json to_json() const;
    static ChatMessage from_json(const nlohmann::json& j);
};

// ---- Tool-call format (S4.N) ------------------------------------------------

// Wire format the model uses to emit tool calls. LM Studio normally wraps
// every model behind the OpenAI tool API and translates Qwen/Claude-style
// text-channel tool calls back into JSON `tool_calls` deltas, but that
// translation is not always reliable -- some models leak the raw XML
// markers into the content channel. The decoder layer extracts them so
// we don't lose tool intent regardless of who let it through.
//
//   Auto   -- always extract Qwen + Claude markers from text in addition to
//             the JSON tool_calls path. Default.
//   OpenAi -- trust LM Studio's JSON tool_calls deltas only; skip XML
//             extraction. Slightly cheaper, useful when the user knows
//             the model behaves.
//   Qwen   -- run JSON path + Qwen-only XML extraction (`<tool_call>...`).
//   Claude -- run JSON path + Claude-only XML extraction (`<function_calls>...`).
//   None   -- omit the `tools` array from the request entirely; the model
//             is expected not to call tools (e.g. base Llama3 untrained).
enum class ToolFormat {
    Auto,
    OpenAi,
    Qwen,
    Claude,
    None
};

const char* to_string(ToolFormat f);
ToolFormat  tool_format_from_string(const std::string& s);

// ---- Grammar mode (S6.10 Task D) --------------------------------------------
//
// Server-side grammar-constrained decoding for tool calls. When the server
// supports it (LM Studio / llama.cpp / vLLM all do via `response_format` with
// a JSON schema), attaching a schema to the request body constrains the
// sampler so malformed tool-call JSON becomes physically impossible to
// generate. Task A's repair pre-pass is still the universal fallback for
// servers that don't honour the schema.
//
//   Off         -- never attach a constraint. The default until a preset opts
//                  in -- preserves the pre-S6.10 behaviour exactly.
//   BestEffort  -- attach a `response_format: {type: "json_schema", ...}`
//                  describing the union of valid tool calls. Servers that
//                  honour it constrain the tokens; servers that don't ignore
//                  the field. Locus's stream decoder + Task A repair still
//                  run either way.
//   Strict      -- same payload as BestEffort today; reserved for a future
//                  startup-time server probe that refuses to start the
//                  session when the server cannot honour the schema. Until
//                  the probe lands, treat as BestEffort.
enum class GrammarMode {
    Off,
    BestEffort,
    Strict
};

const char* to_string(GrammarMode m);
GrammarMode grammar_mode_from_string(const std::string& s);

// ---- Config -----------------------------------------------------------------

struct LLMConfig {
    std::string base_url      = "http://127.0.0.1:1234";
    std::string model         = "";          // empty = use server default
    double      temperature   = 0.7;
    int         max_tokens    = 8192;
    // 0 sentinel = "ask the server", resolved by LocusSession at startup
    // from the LM Studio / Ollama / llama-server model-info endpoint.
    // Hardcoding 8192 here used to silently cap 32k-context models to 8k
    // and trigger spurious auto-compaction on big context windows.
    int         context_limit = 0;

    int         timeout_ms    = 1800000;     // stream stall timeout: abort if no bytes flow for this long. Not a total-request cap -- long reasoning streams are fine. 1800s default covers prefill + a buffered <think> block on a 27B+ thinking model with multi-K-token context on a consumer GPU running at low t/s; configurable per workspace via .locus/config.json llm.timeout_ms. Was 600s pre-S5; bumped after agentic testing (tests/ui_automation/output/agentic_Tetris/findings.md) showed qwen3.6-27b reasoning rounds routinely exceeded the old default on tool-call-heavy payloads.
    ToolFormat  tool_format   = ToolFormat::Auto;

    // S4.V Task 7 -- optional samplers. Sentinel 0 means "don't include in
    // request, let the server use its own default" (most servers default to
    // sensible values per model; sending unwanted overrides is worse than
    // sending nothing). Field names match what llama.cpp / LM Studio /
    // Ollama recognise (`top_k`, `min_p`, `repeat_penalty` are llama.cpp
    // extensions; pure-OpenAI servers ignore unknown fields).
    double      top_p           = 0.0;
    int         top_k           = 0;
    double      min_p           = 0.0;
    double      repeat_penalty  = 0.0;

    // OpenAI-protocol penalties (separate from llama.cpp's `repeat_penalty`).
    // Range -2.0 to 2.0 per the OpenAI / NVIDIA spec; 0.0 sentinel means
    // "don't include in the request". LM Studio + Ollama + llama-server all
    // accept them. Note: these are *additive* penalties (the OpenAI spec uses
    // logits subtraction), whereas `repeat_penalty` is multiplicative -- the
    // three knobs compose rather than overlap.
    double      frequency_penalty = 0.0;
    double      presence_penalty  = 0.0;

    // S6.10 Task D -- server-side grammar-constrained decoding for tool calls.
    // Default Off preserves pre-S6.10 behaviour. Set to BestEffort to attach a
    // `response_format` json_schema describing the union of valid tool calls
    // (compatible with LM Studio / llama.cpp / vLLM; ignored by servers that
    // don't honour the field).
    GrammarMode grammar_mode = GrammarMode::Off;

    // S6.16 -- endpoint-profile auth. `api_key` (when non-empty) becomes an
    // `Authorization: Bearer <key>` header; empty = no Authorization header
    // (local LM Studio / Ollama). `extra_headers` are appended verbatim to
    // the request header block (e.g. OpenRouter's HTTP-Referer / X-Title).
    // The key is never logged at any level -- see OpenAiTransport.
    std::string api_key;
    std::map<std::string, std::string> extra_headers;
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
    int cached_tokens     = 0;   // subset of prompt_tokens served from prompt cache
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

// S6.21 Task 1 -- delivered/dropped tool-call accounting for one stream.
// `delivered` = calls that passed the JSON-arguments gate and were handed to
// on_tool_calls; `dropped` = calls the gate rejected as malformed. Fired once
// per stream IFF at least one tool call was accumulated (delivered+dropped>0),
// so AgentTurnRunner can distinguish "no tool calls at all" (delivered=0,
// dropped=0, callback not fired) from "every tool call was malformed"
// (delivered=0, dropped>=1). `diagnostic` is a compact, already-bounded
// human-readable summary of WHY the dropped calls failed (which tool, reason,
// arg size) -- Task 1b folds this into the user-visible error too. Empty =
// ignored; default-empty so existing callers are unaffected.
using OnToolCallsAccounting =
    std::function<void(int delivered, int dropped, const std::string& diagnostic)>;

struct StreamCallbacks {
    OnToken     on_token;
    OnToken     on_reasoning_token;   // chain-of-thought tokens (LM Studio's
                                      // reasoning_content); empty = ignored
    OnToolCalls on_tool_calls;
    OnComplete  on_complete;
    OnError     on_error;
    OnUsage     on_usage;

    // S6.21 Task 1 -- see OnToolCallsAccounting above.
    OnToolCallsAccounting on_tool_calls_accounting;

    // S6.20 -- transient-failure status notice. Fired when the transport is
    // about to retry a transient failure (429 / 5xx / empty-200 / stall) after
    // a backoff. `message` is a short human-readable status (e.g.
    // "rate limited -- retrying 2/5 in 8s"). Frontends surface it in the chat
    // footer status line so a multi-minute backoff isn't silent. Cleared when
    // the stream resumes (the next on_token / on_complete / on_error). Empty
    // = ignored. Default-empty so existing callers are unaffected.
    std::function<void(const std::string& message)> on_status;

    // Polled inside the streaming hot path. Return true to request an
    // immediate abort of the HTTP transfer -- the transport returns without
    // an error callback, the partial accumulated text is preserved, and
    // post-stream warnings (max_tokens / content_filter) are suppressed.
    // Used by the GUI Stop button.
    std::function<bool()> on_should_cancel;
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

// S6.16 -- join an OpenAI-compatible base_url with an API path, tolerating a
// base_url that already ends in "/v1". LM Studio's base is host-only
// ("http://127.0.0.1:1234") so the caller appends "/v1/chat/completions";
// hosted providers (NVIDIA / Ollama / OpenRouter) document their base WITH the
// "/v1" suffix ("https://integrate.api.nvidia.com/v1"). Without this the path
// doubles to ".../v1/v1/chat/completions" and 404s. `path` is expected to lead
// with "/v1/" (e.g. "/v1/chat/completions", "/v1/models"); a base already
// ending in "/v1" gets the leading "/v1" trimmed from the join. Trailing
// slashes on base are normalised away.
inline std::string join_api_url(std::string base, const std::string& path)
{
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (path.rfind("/v1/", 0) == 0 && base.size() >= 3 &&
        base.compare(base.size() - 3, 3, "/v1") == 0)
        return base + path.substr(3);   // base(".../v1") + "/chat/completions"
    return base + path;
}

// Factory: creates a LMStudioClient (OpenAI-compatible) with the given config.
std::unique_ptr<ILLMClient> create_llm_client(LLMConfig config);

} // namespace locus
