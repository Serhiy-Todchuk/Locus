#include "llm/llm_client.h"
#include "llm/openai_transport.h"
#include "llm/stream_decoders/openai_decoder.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

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

    LLMConfig         config_;
    OpenAiTransport   transport_;
    OpenAiDecoder     decoder_;
};

LMStudioClient::LMStudioClient(LLMConfig config)
    : config_(std::move(config))
    , transport_(config_)
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

    // Preferred model not found -- fall back to first.
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
    // Try LM Studio proprietary API first -- it includes loaded_context_length.
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

    // Per-call accumulator: tool-call fragments arrive indexed; we
    // assemble them here and deliver completed calls after the stream
    // ends. The decoder is stateless -- accumulation is the client's job.
    struct ToolCallAccum {
        std::string id;
        std::string name;
        std::string arguments;
    };
    std::vector<ToolCallAccum> tool_accum;
    bool got_tool_calls = false;
    bool got_data       = false;
    bool errored        = false;
    CompletionUsage usage;

    decoder_.reset();

    StreamDecoderSink sink;
    sink.on_text = [&](const std::string& token) {
        if (callbacks.on_token)
            callbacks.on_token(token);
    };
    sink.on_reasoning = [&](const std::string& token) {
        if (callbacks.on_reasoning_token)
            callbacks.on_reasoning_token(token);
    };
    sink.on_tool_call_delta = [&](const StreamDecoderSink::ToolCallDelta& d) {
        got_tool_calls = true;
        while (static_cast<int>(tool_accum.size()) <= d.index)
            tool_accum.push_back({});
        auto& a = tool_accum[d.index];
        if (!d.id_frag.empty())
            a.id = d.id_frag;
        a.name      += d.name_frag;
        a.arguments += d.args_frag;
    };
    sink.on_usage = [&](const CompletionUsage& u) {
        usage = u;
        spdlog::trace("LLM usage: prompt={}, completion={} (reasoning={}), total={}",
                      u.prompt_tokens, u.completion_tokens,
                      u.reasoning_tokens, u.total_tokens);
    };

    OpenAiTransport::Callbacks tcbs;
    tcbs.on_data = [&](const std::string& data) -> bool {
        got_data = true;
        return decoder_.decode(data, sink);
    };
    tcbs.on_error = [&](const std::string& err) {
        errored = true;
        if (callbacks.on_error)
            callbacks.on_error(err);
    };

    transport_.post_chat(body.dump(), tcbs);

    if (errored)
        return;

    // No data received at all (transport ok but server sent empty body).
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
        for (auto& a : tool_accum)
            calls.push_back({a.id, a.name, a.arguments});
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
