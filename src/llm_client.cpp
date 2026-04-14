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
    std::string stream_error;

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

            spdlog::trace("LLM chunk: {}", data);

            // Navigate: choices[0].delta
            if (!delta_json.contains("choices") || delta_json["choices"].empty())
                return true;

            auto& choice = delta_json["choices"][0];

            if (!choice.contains("delta"))
                return true;

            auto& delta = choice["delta"];

            // Text content (some models use "content", reasoning models may use
            // "reasoning_content" for chain-of-thought).
            for (const char* field : {"content", "reasoning_content"}) {
                if (delta.contains(field) && delta[field].is_string()) {
                    std::string token = delta[field].get<std::string>();
                    if (!token.empty() && callbacks.on_token)
                        callbacks.on_token(token);
                }
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
