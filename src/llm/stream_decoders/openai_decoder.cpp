#include "llm/stream_decoders/openai_decoder.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace locus {

namespace {

// Defensive string extract: returns "" if value is null / wrong type.
std::string safe_string(const json& j)
{
    if (j.is_string()) return j.get<std::string>();
    return {};
}

} // namespace

bool OpenAiDecoder::decode(const std::string& payload, const StreamDecoderSink& sink)
{
    try {
        auto delta_json = json::parse(payload);

        // Mid-stream server error: some backends (vLLM, sglang, occasionally
        // LM Studio after a model crash) emit a top-level {"error":{...}}
        // chunk in the SSE feed instead of failing the HTTP request. Surface
        // it on the dedicated channel so the client can stop the stream
        // cleanly with a useful message.
        if (delta_json.contains("error") && sink.on_stream_error) {
            std::string msg;
            auto& err = delta_json["error"];
            if (err.is_object())
                msg = err.value("message", err.dump());
            else if (err.is_string())
                msg = err.get<std::string>();
            else
                msg = err.dump();
            sink.on_stream_error(msg);
            return true;
        }

        // Navigate: choices[0].delta
        if (delta_json.contains("choices") && !delta_json["choices"].empty()) {
            auto& choice = delta_json["choices"][0];
            if (choice.contains("delta")) {
                auto& delta = choice["delta"];

                // Visible answer tokens.
                if (delta.contains("content") && delta["content"].is_string()) {
                    std::string token = delta["content"].get<std::string>();
                    if (!token.empty() && sink.on_text)
                        sink.on_text(token);
                }

                // Chain-of-thought tokens (reasoning models, e.g. Gemma
                // via LM Studio). Routed to a separate channel so the
                // frontend can render them distinctly (or hide them).
                if (delta.contains("reasoning_content") &&
                    delta["reasoning_content"].is_string()) {
                    std::string token = delta["reasoning_content"].get<std::string>();
                    if (!token.empty() && sink.on_reasoning)
                        sink.on_reasoning(token);
                }

                // Refusal channel (Claude / OpenAI emit this when the model
                // declines to answer instead of producing content). We route
                // it through on_text with a clear prefix so the user sees
                // *something* -- otherwise an entire response can be empty.
                if (delta.contains("refusal") &&
                    delta["refusal"].is_string()) {
                    std::string token = delta["refusal"].get<std::string>();
                    if (!token.empty() && sink.on_text)
                        sink.on_text("[refusal] " + token);
                }

                // Tool calls (streamed incrementally -- name and arguments
                // arrive in fragments tagged by index).
                if (delta.contains("tool_calls") && delta["tool_calls"].is_array() &&
                    sink.on_tool_call_delta) {
                    for (auto& tc_delta : delta["tool_calls"]) {
                        StreamDecoderSink::ToolCallDelta d;
                        d.index = tc_delta.value("index", 0);

                        if (tc_delta.contains("id"))
                            d.id_frag = safe_string(tc_delta["id"]);

                        if (tc_delta.contains("function") &&
                            tc_delta["function"].is_object()) {
                            auto& fn = tc_delta["function"];
                            if (fn.contains("name"))
                                d.name_frag = safe_string(fn["name"]);
                            if (fn.contains("arguments"))
                                d.args_frag = safe_string(fn["arguments"]);
                        }

                        sink.on_tool_call_delta(d);
                    }
                }
            }

            // finish_reason rides on the same chunk that closes generation.
            // Most servers send it on the last delta with content/tool_calls
            // empty; some send it together with the last token. Either way,
            // emit only when non-null so callers don't see spurious "" values.
            if (choice.contains("finish_reason") &&
                choice["finish_reason"].is_string() &&
                sink.on_finish_reason) {
                std::string reason = choice["finish_reason"].get<std::string>();
                if (!reason.empty())
                    sink.on_finish_reason(reason);
            }
        }

        // Usage (typically in the final chunk, alongside an empty delta).
        if (delta_json.contains("usage") && delta_json["usage"].is_object() &&
            sink.on_usage) {
            CompletionUsage usage;
            auto& u = delta_json["usage"];
            usage.prompt_tokens     = u.value("prompt_tokens", 0);
            usage.completion_tokens = u.value("completion_tokens", 0);
            usage.total_tokens      = u.value("total_tokens", 0);
            if (u.contains("completion_tokens_details") &&
                u["completion_tokens_details"].is_object()) {
                usage.reasoning_tokens =
                    u["completion_tokens_details"].value("reasoning_tokens", 0);
            }
            if (u.contains("prompt_tokens_details") &&
                u["prompt_tokens_details"].is_object()) {
                usage.cached_tokens =
                    u["prompt_tokens_details"].value("cached_tokens", 0);
            }
            sink.on_usage(usage);
        }
    } catch (const json::exception& e) {
        spdlog::warn("OpenAiDecoder: JSON error in chunk: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("OpenAiDecoder: error processing chunk: {}", e.what());
    }

    return true;
}

} // namespace locus
