#include "llm/llm_client.h"
#include "llm/openai_transport.h"
#include "llm/stream_decoder.h"
#include "llm/stream_decoders/openai_decoder.h"
#include "llm/stream_decoders/qwen_xml_decoder.h"
#include "llm/stream_decoders/claude_xml_decoder.h"
#include "llm/stream_decoders/auto_decoder.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace locus {

// ---- ToolFormat string conversion -------------------------------------------

const char* to_string(ToolFormat f)
{
    switch (f) {
    case ToolFormat::Auto:   return "auto";
    case ToolFormat::OpenAi: return "openai";
    case ToolFormat::Qwen:   return "qwen";
    case ToolFormat::Claude: return "claude";
    case ToolFormat::None:   return "none";
    }
    return "auto";
}

ToolFormat tool_format_from_string(const std::string& s)
{
    std::string l;
    l.reserve(s.size());
    for (char c : s) l.push_back(static_cast<char>(std::tolower(
        static_cast<unsigned char>(c))));
    if (l == "openai" || l == "open_ai" || l == "open-ai") return ToolFormat::OpenAi;
    if (l == "qwen")                                       return ToolFormat::Qwen;
    if (l == "claude" || l == "anthropic" || l == "xml")   return ToolFormat::Claude;
    if (l == "none" || l == "off" || l == "disabled")      return ToolFormat::None;
    // Unknown values fall back to Auto so a typo never breaks tool calls.
    return ToolFormat::Auto;
}

namespace {

// Build the right decoder for the configured tool_format. Defined in this
// TU so that the LMStudioClient (also in this TU) can hold a generic
// std::unique_ptr<IStreamDecoder> instead of a templated member.
std::unique_ptr<IStreamDecoder> make_decoder_for(ToolFormat tf)
{
    switch (tf) {
    case ToolFormat::OpenAi:
    case ToolFormat::None:
        return std::make_unique<OpenAiDecoder>();
    case ToolFormat::Qwen:
        return std::make_unique<QwenXmlDecoder>();
    case ToolFormat::Claude:
        return std::make_unique<ClaudeXmlDecoder>();
    case ToolFormat::Auto:
    default:
        return std::make_unique<AutoToolFormatDecoder>();
    }
}

} // namespace

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

    LLMConfig                       config_;
    OpenAiTransport                 transport_;
    std::unique_ptr<IStreamDecoder> decoder_;
};

LMStudioClient::LMStudioClient(LLMConfig config)
    : config_(std::move(config))
    , transport_(config_)
    , decoder_(make_decoder_for(config_.tool_format))
{
    // Strip trailing slash from base URL.
    while (!config_.base_url.empty() && config_.base_url.back() == '/')
        config_.base_url.pop_back();

    spdlog::info("LLM client: endpoint={} model={} temp={} max_tokens={} tool_format={}",
                 config_.base_url,
                 config_.model.empty() ? "(server default)" : config_.model,
                 config_.temperature,
                 config_.max_tokens,
                 to_string(config_.tool_format));
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

    // Tool definitions. Skipped entirely when tool_format=None -- the model
    // is expected not to call tools (e.g. base Llama3 with no tool training).
    if (!tools.empty() && config_.tool_format != ToolFormat::None) {
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
    std::string finish_reason;
    CompletionUsage usage;

    decoder_->reset();

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
        int slot = d.index;
        while (static_cast<int>(tool_accum.size()) <= slot)
            tool_accum.push_back({});

        // Some chat templates (notably certain Qwen/Gemma builds in LM Studio)
        // keep `index` at 0 for *sequential* tool calls in one assistant
        // turn, instead of incrementing it. Without this guard the
        // accumulator concatenates the names ("write_file" + "delete_file" =
        // "write_filedelete_file") and the arguments, sending a malformed
        // call to the dispatcher. Detect via two signals:
        //   1. id_frag is non-empty and differs from the slot's recorded id
        //      (the cleanest signal; ids are unique per call).
        //   2. a non-empty name_frag arrives after the slot already collected
        //      arguments (once arguments started streaming, the name is
        //      complete -- a fresh name means a fresh call).
        bool different_id =
            !d.id_frag.empty() && !tool_accum[slot].id.empty() &&
            tool_accum[slot].id != d.id_frag;
        bool name_after_args =
            !d.name_frag.empty() && !tool_accum[slot].arguments.empty();
        if (different_id || name_after_args) {
            spdlog::warn(
                "LMStudioClient: tool call delta at reused index {} "
                "(prev id='{}' name='{}', args_len={}, new id_frag='{}', "
                "name_frag='{}'); allocating a new slot",
                d.index, tool_accum[slot].id, tool_accum[slot].name,
                tool_accum[slot].arguments.size(), d.id_frag, d.name_frag);
            slot = static_cast<int>(tool_accum.size());
            tool_accum.push_back({});
        }

        auto& a = tool_accum[slot];
        if (!d.id_frag.empty())
            a.id = d.id_frag;
        a.name      += d.name_frag;
        a.arguments += d.args_frag;
    };
    sink.on_usage = [&](const CompletionUsage& u) {
        usage = u;
        spdlog::trace("LLM usage: prompt={} (cached={}), completion={} "
                      "(reasoning={}), total={}",
                      u.prompt_tokens, u.cached_tokens,
                      u.completion_tokens, u.reasoning_tokens, u.total_tokens);
    };
    sink.on_finish_reason = [&](const std::string& reason) {
        // Last writer wins; servers normally emit at most one non-null
        // finish_reason per stream, but be defensive against backends
        // that resend on the trailing chunk.
        finish_reason = reason;
        spdlog::trace("LLM finish_reason: {}", reason);
    };
    sink.on_stream_error = [&](const std::string& message) {
        std::string err = "LLM stream error: " + message;
        spdlog::error("{}", err);
        errored = true;
        if (callbacks.on_error)
            callbacks.on_error(err);
    };

    OpenAiTransport::Callbacks tcbs;
    tcbs.on_data = [&](const std::string& data) -> bool {
        got_data = true;
        if (errored)
            return false;  // stop feeding the decoder once we've raised
        return decoder_->decode(data, sink);
    };
    tcbs.on_error = [&](const std::string& err) {
        errored = true;
        if (callbacks.on_error)
            callbacks.on_error(err);
    };

    transport_.post_chat(body.dump(), tcbs);

    if (errored)
        return;

    // Flush any state the decoder is still holding back (XML decoders
    // may have buffered a few bytes waiting on a partial-tag suffix).
    decoder_->finish_stream(sink);

    // No data received at all (transport ok but server sent empty body).
    if (!got_data) {
        std::string err_msg = "LLM server returned empty response";
        spdlog::error("{}", err_msg);
        if (callbacks.on_error)
            callbacks.on_error(err_msg);
        return;
    }

    // S4.U -- max_tokens truncation. The server signals it via
    // finish_reason="length"; we noticed the bug in the wild because the
    // model emitted two write_file tool calls and the second was cut
    // mid-arguments, leaving an empty arguments string that broke the next
    // chat-template render with HTTP 500. Surface it loud and early so the
    // user can bump the limit instead of staring at a cryptic 500.
    bool truncated_by_length = (finish_reason == "length");
    if (truncated_by_length) {
        std::string warn_msg =
            "Response truncated: max_tokens limit reached (max_tokens=" +
            std::to_string(config_.max_tokens) +
            ", completion_tokens=" + std::to_string(usage.completion_tokens) +
            "). Increase llm.max_tokens in .locus/config.json or in "
            "Settings -> LLM, or shorten the request.";
        spdlog::warn("{}", warn_msg);
        if (callbacks.on_error)
            callbacks.on_error(warn_msg);
    } else if (!finish_reason.empty()
               && finish_reason != "stop"
               && finish_reason != "tool_calls"
               && finish_reason != "function_call") {
        // Anything else non-trivial (e.g. "content_filter") -- surface it
        // too so the user knows why the response stopped.
        std::string warn_msg = "LLM stop reason: " + finish_reason;
        spdlog::warn("{}", warn_msg);
        if (callbacks.on_error)
            callbacks.on_error(warn_msg);
    }

    // Deliver accumulated tool calls. Validate each call's arguments
    // string parses as JSON before handing off; a truncated stream (case
    // above) commonly leaves the trailing call with arguments="" or a
    // half-finished JSON fragment. Letting that into ConversationHistory
    // poisons the next request -- LM Studio's chat template can't render
    // a malformed tool_calls entry and answers HTTP 500 on the round
    // after. The XML decoders already validate their bodies inside the
    // extractor, so this gate primarily catches OpenAI native truncation.
    if (got_tool_calls && !tool_accum.empty() && callbacks.on_tool_calls) {
        std::vector<ToolCallRequest> calls;
        int dropped = 0;
        for (auto& a : tool_accum) {
            // Empty / whitespace arguments => not deliverable.
            std::string args = a.arguments;
            std::string trimmed = args;
            auto is_ws = [](char c) {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r';
            };
            while (!trimmed.empty() && is_ws(trimmed.front())) trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && is_ws(trimmed.back()))  trimmed.pop_back();

            bool valid = true;
            if (a.name.empty()) {
                valid = false;
            } else if (trimmed.empty()) {
                // Tools that legitimately take no args still expect "{}".
                // An empty arguments string poisons LM Studio's chat
                // template on the next round.
                valid = false;
            } else {
                try { (void)json::parse(trimmed); }
                catch (const json::exception&) { valid = false; }
            }

            if (!valid) {
                ++dropped;
                spdlog::warn("LMStudioClient: dropping malformed tool call "
                             "(name='{}', id='{}', args_len={}): args do not "
                             "parse as JSON",
                             a.name, a.id, a.arguments.size());
                continue;
            }
            calls.push_back({a.id, a.name, a.arguments});
        }

        if (dropped > 0) {
            std::string err_msg =
                "Dropped " + std::to_string(dropped) +
                " malformed tool call(s) from the model response";
            if (truncated_by_length)
                err_msg += " (likely caused by max_tokens truncation; "
                           "increase llm.max_tokens to recover)";
            err_msg += ".";
            if (callbacks.on_error)
                callbacks.on_error(err_msg);
        }

        spdlog::trace("LLM tool calls: {} delivered, {} dropped",
                      calls.size(), dropped);
        for (auto& c : calls)
            spdlog::trace("  tool: {} id={} args={}", c.name, c.id, c.arguments);

        if (!calls.empty())
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
