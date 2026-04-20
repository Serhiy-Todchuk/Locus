#include "llm_client.h"
#include "sse_parser.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <stdexcept>

using json = nlohmann::json;

namespace locus {

// ---- ChatMessage JSON -------------------------------------------------------

json ChatMessage::to_json() const
{
    json j;
    j["role"] = to_string(role);

    if (!content.empty())
        j["content"] = content;

    if (!tool_calls.empty()) {
        json arr = json::array();
        for (auto& tc : tool_calls) {
            arr.push_back({
                {"id",   tc.id},
                {"type", "function"},
                {"function", {
                    {"name",      tc.name},
                    {"arguments", tc.arguments}
                }}
            });
        }
        j["tool_calls"] = std::move(arr);
    }

    if (!tool_call_id.empty())
        j["tool_call_id"] = tool_call_id;

    return j;
}

ChatMessage ChatMessage::from_json(const json& j)
{
    ChatMessage m;
    m.role = role_from_string(j.value("role", "user"));
    m.content = j.value("content", "");

    if (j.contains("tool_calls")) {
        for (auto& tc : j["tool_calls"]) {
            ToolCallRequest req;
            req.id   = tc.value("id", "");
            auto& fn = tc["function"];
            req.name = fn.value("name", "");
            req.arguments = fn.value("arguments", "");
            m.tool_calls.push_back(std::move(req));
        }
    }

    m.tool_call_id = j.value("tool_call_id", "");
    return m;
}

// ---- Token estimation -------------------------------------------------------

int ILLMClient::estimate_tokens(const std::string& text)
{
    // Heuristic: ~4 characters per token (good enough for local models).
    return static_cast<int>((text.size() + 3) / 4);
}

int ILLMClient::estimate_tokens(const std::vector<ChatMessage>& messages)
{
    int total = 0;
    for (auto& m : messages) {
        // Role overhead: ~4 tokens per message framing.
        total += 4;
        total += estimate_tokens(m.content);
        for (auto& tc : m.tool_calls) {
            total += estimate_tokens(tc.name);
            total += estimate_tokens(tc.arguments);
        }
    }
    return total;
}

// ---- LMStudioClient ---------------------------------------------------------

class LMStudioClient : public ILLMClient {
public:
    explicit LMStudioClient(LLMConfig config);

    void stream_completion(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolSchema>&  tools,
        const StreamCallbacks&          callbacks) override;

    ModelInfo query_model_info() override;

private:
    json build_request_body(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolSchema>&  tools) const;

    void do_stream(const json& body, const StreamCallbacks& callbacks, bool is_retry);

    LLMConfig config_;
};

LMStudioClient::LMStudioClient(LLMConfig config)
    : config_(std::move(config))
{
    // Strip trailing slash from base URL.
    while (!config_.base_url.empty() && config_.base_url.back() == '/')
        config_.base_url.pop_back();

    spdlog::info("LLM client: endpoint={} model={} temp={} max_tokens={}",
                 config_.base_url,
                 config_.model.empty() ? "(server default)" : config_.model,
                 config_.temperature,
                 config_.max_tokens);
}

// Helper: extract context length from a model JSON object using known field names.
static int extract_context_length(const json& model)
{
    // LM Studio proprietary: loaded_context_length (actual), max_context_length (ceiling).
    if (model.contains("loaded_context_length") && model["loaded_context_length"].is_number())
        return model["loaded_context_length"].get<int>();
    if (model.contains("max_context_length") && model["max_context_length"].is_number())
        return model["max_context_length"].get<int>();
    // OpenAI / vLLM / other servers.
    if (model.contains("context_length") && model["context_length"].is_number())
        return model["context_length"].get<int>();
    if (model.contains("max_model_len") && model["max_model_len"].is_number())
        return model["max_model_len"].get<int>();
    return 0;
}

// Helper: find a model in a JSON data array, preferring config_.model if set.
static ModelInfo find_model_in(const json& data, const std::string& preferred_model)
{
    // Try to match the preferred model first.
    for (auto& model : data) {
        std::string id = model.value("id", "");
        if (!preferred_model.empty() && id != preferred_model)
            continue;

        ModelInfo info;
        info.id = id;
        info.context_length = extract_context_length(model);
        return info;
    }

    // Preferred model not found — fall back to first.
    if (!data.empty()) {
        auto& first = data[0];
        ModelInfo info;
        info.id = first.value("id", "");
        info.context_length = extract_context_length(first);
        return info;
    }

    return {};
}

ModelInfo LMStudioClient::query_model_info()
{
    // Try LM Studio proprietary API first — it includes loaded_context_length.
    {
        std::string url = config_.base_url + "/api/v0/models";
        spdlog::trace("LLM GET {}", url);

        cpr::Response response = cpr::Get(
            cpr::Url{url},
            cpr::Header{{"Accept", "application/json"}},
            cpr::Timeout{5000}
        );

        if (response.error.code == cpr::ErrorCode::OK && response.status_code == 200) {
            try {
                auto j = json::parse(response.text);
                if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
                    // Filter to loaded models only (LM Studio marks state).
                    json loaded = json::array();
                    for (auto& m : j["data"]) {
                        if (m.value("state", "") == "loaded")
                            loaded.push_back(m);
                    }
                    // Use loaded models if any, otherwise all.
                    auto& candidates = loaded.empty() ? j["data"] : loaded;
                    auto info = find_model_in(candidates, config_.model);
                    if (!info.id.empty()) {
                        spdlog::info("Model info (LM Studio API): id={}, context_length={}",
                                     info.id, info.context_length);
                        return info;
                    }
                }
            } catch (const json::exception& e) {
                spdlog::trace("LM Studio /api/v0/models parse failed: {}", e.what());
            }
        } else {
            spdlog::trace("LM Studio /api/v0/models not available, trying /v1/models");
        }
    }

    // Fallback: standard OpenAI-compatible /v1/models.
    {
        std::string url = config_.base_url + "/v1/models";
        spdlog::trace("LLM GET {}", url);

        cpr::Response response = cpr::Get(
            cpr::Url{url},
            cpr::Header{{"Accept", "application/json"}},
            cpr::Timeout{5000}
        );

        if (response.error.code != cpr::ErrorCode::OK) {
            spdlog::warn("Failed to query /v1/models: {}", response.error.message);
            return {};
        }
        if (response.status_code != 200) {
            spdlog::warn("GET /v1/models returned HTTP {}", response.status_code);
            return {};
        }

        try {
            auto j = json::parse(response.text);
            if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
                spdlog::warn("/v1/models: no models in response");
                return {};
            }

            auto info = find_model_in(j["data"], config_.model);
            spdlog::info("Model info: id={}, context_length={}", info.id, info.context_length);
            return info;

        } catch (const json::exception& e) {
            spdlog::warn("Failed to parse /v1/models response: {}", e.what());
            return {};
        }
    }
}

json LMStudioClient::build_request_body(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolSchema>&  tools) const
{
    json body;

    // Messages array
    json msgs = json::array();
    for (auto& m : messages)
        msgs.push_back(m.to_json());
    body["messages"] = std::move(msgs);

    body["stream"] = true;
    body["stream_options"] = {{"include_usage", true}};
    body["temperature"] = config_.temperature;
    body["max_tokens"]  = config_.max_tokens;

    if (!config_.model.empty())
        body["model"] = config_.model;

    // Tool definitions
    if (!tools.empty()) {
        json tool_arr = json::array();
        for (auto& t : tools) {
            tool_arr.push_back({
                {"type", "function"},
                {"function", {
                    {"name",        t.name},
                    {"description", t.description},
                    {"parameters",  t.parameters}
                }}
            });
        }
        body["tools"] = std::move(tool_arr);
    }

    return body;
}

void LMStudioClient::stream_completion(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolSchema>&  tools,
    const StreamCallbacks&          callbacks)
{
    json body = build_request_body(messages, tools);
    spdlog::trace("LLM request: {} messages, {} tools", messages.size(), tools.size());
    do_stream(body, callbacks, false);
}

void LMStudioClient::do_stream(
    const json& body, const StreamCallbacks& callbacks, bool is_retry)
{
    std::string url = config_.base_url + "/v1/chat/completions";
    std::string body_str = body.dump();

    spdlog::trace("LLM POST {}", url);

    // Accumulator for tool calls across SSE events.
    // Key: tool call index → partially accumulated call.
    struct ToolCallAccum {
        std::string id;
        std::string name;
        std::string arguments;
    };
    std::vector<ToolCallAccum> tool_accum;
    bool got_tool_calls = false;

    // Track whether we received any data at all (for error detection).
    bool got_data = false;
    CompletionUsage usage;

    // Helper: safely extract a string from a JSON value that might be null or wrong type.
    auto safe_string = [](const json& j) -> std::string {
        if (j.is_string()) return j.get<std::string>();
        return {};
    };

    // SSE parser feeds on raw response bytes.
    SseParser parser([&](const std::string& data) -> bool {
        got_data = true;

        if (data == "[DONE]") {
            spdlog::trace("LLM stream: [DONE]");
            return false;  // stop parsing
        }

        try {
            auto delta_json = json::parse(data);

            // Navigate: choices[0].delta
            if (!delta_json.contains("choices") || delta_json["choices"].empty())
                return true;

            auto& choice = delta_json["choices"][0];

            if (!choice.contains("delta"))
                return true;

            auto& delta = choice["delta"];

            // Visible answer tokens.
            if (delta.contains("content") && delta["content"].is_string()) {
                std::string token = delta["content"].get<std::string>();
                if (!token.empty() && callbacks.on_token)
                    callbacks.on_token(token);
            }

            // Chain-of-thought tokens (reasoning models, e.g. Gemma via
            // LM Studio). Routed to a separate callback so the frontend can
            // render them distinctly (or hide them).
            if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                std::string token = delta["reasoning_content"].get<std::string>();
                if (!token.empty() && callbacks.on_reasoning_token)
                    callbacks.on_reasoning_token(token);
            }

            // Tool calls (streamed incrementally)
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                got_tool_calls = true;
                for (auto& tc_delta : delta["tool_calls"]) {
                    int idx = tc_delta.value("index", 0);

                    // Grow accumulator if needed.
                    while (static_cast<int>(tool_accum.size()) <= idx)
                        tool_accum.push_back({});

                    auto& accum = tool_accum[idx];

                    if (tc_delta.contains("id"))
                        accum.id = safe_string(tc_delta["id"]);

                    if (tc_delta.contains("function") && tc_delta["function"].is_object()) {
                        auto& fn = tc_delta["function"];
                        if (fn.contains("name"))
                            accum.name += safe_string(fn["name"]);
                        if (fn.contains("arguments"))
                            accum.arguments += safe_string(fn["arguments"]);
                    }
                }
            }

            // Usage (typically in the final chunk).
            if (delta_json.contains("usage") && delta_json["usage"].is_object()) {
                auto& u = delta_json["usage"];
                usage.prompt_tokens     = u.value("prompt_tokens", 0);
                usage.completion_tokens = u.value("completion_tokens", 0);
                usage.total_tokens      = u.value("total_tokens", 0);
                if (u.contains("completion_tokens_details") &&
                    u["completion_tokens_details"].is_object()) {
                    usage.reasoning_tokens =
                        u["completion_tokens_details"].value("reasoning_tokens", 0);
                }
                spdlog::trace("LLM usage: prompt={}, completion={} (reasoning={}), total={}",
                              usage.prompt_tokens, usage.completion_tokens,
                              usage.reasoning_tokens, usage.total_tokens);
            }
        } catch (const json::exception& e) {
            spdlog::warn("LLM stream: JSON error in chunk: {}", e.what());
        } catch (const std::exception& e) {
            spdlog::warn("LLM stream: error processing chunk: {}", e.what());
        }

        return true;
    });

    // Use cpr WriteCallback to feed SSE parser incrementally.
    cpr::WriteCallback write_cb{[&](std::string_view data,
                                    intptr_t /*userdata*/) -> bool {
        parser.feed(std::string(data));
        return true;
    }};

    cpr::Response response = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Content-Type", "application/json"},
            {"Accept",       "text/event-stream"}
        },
        cpr::Body{body_str},
        cpr::Timeout{static_cast<std::int32_t>(config_.timeout_ms)},
        std::move(write_cb)
    );

    // Flush any remaining data in SSE buffer.
    parser.finish();

    // Check for transport-level errors.
    if (response.error.code != cpr::ErrorCode::OK) {
        std::string err_msg;
        if (response.error.code == cpr::ErrorCode::COULDNT_CONNECT) {
            err_msg = "Cannot connect to LLM server at " + config_.base_url +
                      " - is LM Studio running?";
        } else if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
            if (!is_retry) {
                spdlog::warn("LLM request timed out, retrying once...");
                do_stream(body, callbacks, true);
                return;
            }
            err_msg = "LLM request timed out after retry";
        } else {
            err_msg = "LLM request failed: " + response.error.message;
        }

        spdlog::error("{}", err_msg);
        if (callbacks.on_error)
            callbacks.on_error(err_msg);
        return;
    }

    // Check HTTP status.
    if (response.status_code != 200) {
        std::string err_msg = "LLM server returned HTTP " +
                              std::to_string(response.status_code);
        if (!response.text.empty())
            err_msg += ": " + response.text.substr(0, 500);

        spdlog::error("{}", err_msg);
        if (callbacks.on_error)
            callbacks.on_error(err_msg);
        return;
    }

    // No data received at all.
    if (!got_data) {
        std::string err_msg = "LLM server returned empty response";
        spdlog::error("{}", err_msg);
        if (callbacks.on_error)
            callbacks.on_error(err_msg);
        return;
    }

    // Deliver accumulated tool calls.
    if (got_tool_calls && !tool_accum.empty() && callbacks.on_tool_calls) {
        std::vector<ToolCallRequest> calls;
        for (auto& a : tool_accum) {
            calls.push_back({a.id, a.name, a.arguments});
        }
        spdlog::trace("LLM tool calls: {}", calls.size());
        for (auto& c : calls)
            spdlog::trace("  tool: {} id={} args={}", c.name, c.id, c.arguments);
        callbacks.on_tool_calls(calls);
    }

    // Deliver usage if available.
    if (usage.total_tokens > 0 && callbacks.on_usage)
        callbacks.on_usage(usage);

    // Signal completion.
    if (callbacks.on_complete)
        callbacks.on_complete();
}

// ---- Factory ----------------------------------------------------------------

std::unique_ptr<ILLMClient> create_llm_client(LLMConfig config)
{
    return std::make_unique<LMStudioClient>(std::move(config));
}

} // namespace locus
