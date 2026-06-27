#include "llm/thinking_injection.h"

#include "llm/llm_client.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <string>

namespace locus {

namespace {

std::string lower_strip_publisher(std::string_view model_id)
{
    std::string id(model_id);
    if (auto slash = id.find('/'); slash != std::string::npos)
        id = id.substr(slash + 1);
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return id;
}

// Warn at most once per (model family) so a long agentic run on an unknown
// model doesn't spam the log every round. Keyed by the lower-cased id so two
// different unknown models each warn once.
void warn_unknown_once(const std::string& id, ThinkingMode mode)
{
    static std::mutex      mtx;
    static std::set<std::string> warned;
    std::lock_guard<std::mutex> lk(mtx);
    if (!warned.insert(id).second) return;
    spdlog::warn("thinking_mode forced to {} but no known mechanism for model "
                 "'{}'; ignoring",
                 mode == ThinkingMode::On ? "On" : "Off", id);
}

}  // namespace

ThinkingFamily classify_thinking_family(std::string_view model_id)
{
    const std::string id = lower_strip_publisher(model_id);
    if (id.empty()) return ThinkingFamily::Unknown;

    auto has = [&](std::string_view needle) {
        return id.find(needle) != std::string::npos;
    };

    // Most-specific first. DeepSeek-R1 and o1 are reasoning-effort models; they
    // must win before any generic qwen rule (deepseek-r1-distill-qwen carries
    // "qwen" in the id). The Qwen3 hybrid rule must win over the legacy qwen
    // rule, which is why it is checked before it.
    if (has("deepseek-r1") || has("deepseek_r1") || has("deepseek.r1"))
        return ThinkingFamily::ReasoningEffort;
    // o1 / o3 OpenAI-style reasoning models. Guard against matching a bare "o1"
    // inside an unrelated word by requiring it at a token boundary-ish position.
    if (has("o1-") || id == "o1" || has("o1_") ||
        has("o3-") || id == "o3" || has("o3_"))
        return ThinkingFamily::ReasoningEffort;

    // Qwen 3.x hybrid thinking models. "qwen3" / "qwen-3" / "qwen3.6" plus the
    // explicit "*-thinking-*" tag some builds carry.
    if (has("qwen3") || has("qwen-3") || has("qwen_3") ||
        (has("qwen") && has("thinking")))
        return ThinkingFamily::Qwen3Hybrid;

    // Qwen 2.x / older qwen-* (qwen2, qwen2.5, bare qwen-*). These honour the
    // /no_think /think message directive rather than the template kwarg.
    if (has("qwen2") || has("qwen-2") || has("qwen_2") || has("qwen"))
        return ThinkingFamily::Qwen2Legacy;

    return ThinkingFamily::Unknown;
}

void apply_thinking_mode(nlohmann::json&           request,
                         std::vector<ChatMessage>& messages,
                         ThinkingMode              mode,
                         std::string_view          model_id)
{
    // Auto is the server-default behaviour -- never touch the request.
    if (mode == ThinkingMode::Auto) return;

    const bool on = (mode == ThinkingMode::On);

    switch (classify_thinking_family(model_id)) {
    case ThinkingFamily::Qwen3Hybrid: {
        // LM Studio / vLLM read enable_thinking out of chat_template_kwargs.
        // Some servers also accept it nested under extra_body; chat_template_kwargs
        // is the documented top-level key for LM Studio's Qwen3 templates.
        request["chat_template_kwargs"]["enable_thinking"] = on;
        break;
    }
    case ThinkingFamily::ReasoningEffort: {
        // o1 / DeepSeek-R1: the OpenAI-protocol reasoning_effort knob. "low"
        // is the closest thing to "off" these models expose (they always do
        // SOME reasoning); "high" maxes it out.
        request["reasoning_effort"] = on ? "high" : "low";
        break;
    }
    case ThinkingFamily::Qwen2Legacy: {
        // Append the /think or /no_think directive to the last user message.
        // Mutate the vector and re-publish request["messages"] so both stay in
        // sync regardless of whether the caller built the body's messages array
        // before or after this call.
        const std::string suffix = on ? "\n/think" : "\n/no_think";
        int last_user = -1;
        for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
            if (messages[static_cast<size_t>(i)].role == MessageRole::user) {
                last_user = i;
                break;
            }
        }
        if (last_user >= 0) {
            auto& msg = messages[static_cast<size_t>(last_user)];
            msg.content += suffix;
            // Re-publish into the request body if it already carries messages.
            if (request.contains("messages") && request["messages"].is_array()) {
                request["messages"] = nlohmann::json::array();
                for (const auto& m : messages)
                    request["messages"].push_back(m.to_json());
            }
        }
        break;
    }
    case ThinkingFamily::Unknown:
        warn_unknown_once(lower_strip_publisher(model_id), mode);
        break;
    }
}

} // namespace locus
