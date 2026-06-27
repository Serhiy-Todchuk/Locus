// S6.14 -- thinking ON/OFF/auto injection unit tests.
//
// Covers the pure apply_thinking_mode + classify_thinking_family helpers:
//   - Qwen 3.x off  -> request.chat_template_kwargs.enable_thinking == false
//   - Qwen 2.x off  -> last user message gains a \n/no_think suffix
//   - o1 / R1 off   -> request.reasoning_effort == "low"
//   - Auto, any     -> request unchanged (round-trip equality)
//   - Unknown + Off -> request unchanged + a warn is logged
//
// The helpers are pure (no I/O beyond the one warn log), so these run with the
// rest of the fast hermetic suite.

#include <catch2/catch_test_macros.hpp>

#include "llm/llm_client.h"
#include "llm/thinking_injection.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <memory>
#include <string>
#include <vector>

using namespace locus;
using json = nlohmann::json;

namespace {

// Build a request body the way LMStudioClient::build_request_body does -- a
// "messages" array plus the usual scalars -- so the helper sees a realistic
// shape and the Qwen2 re-publish path has something to rewrite.
json make_request(const std::vector<ChatMessage>& messages)
{
    json body;
    json msgs = json::array();
    for (const auto& m : messages)
        msgs.push_back(m.to_json());
    body["messages"] = std::move(msgs);
    body["stream"] = true;
    body["temperature"] = 0.7;
    return body;
}

std::vector<ChatMessage> sys_and_user(const std::string& user_text)
{
    std::vector<ChatMessage> v;
    ChatMessage sys;  sys.role = MessageRole::system; sys.content = "You are Locus.";
    ChatMessage usr;  usr.role = MessageRole::user;   usr.content = user_text;
    v.push_back(std::move(sys));
    v.push_back(std::move(usr));
    return v;
}

}  // namespace

TEST_CASE("classify_thinking_family resolves families by id", "[s6.14][thinking]")
{
    using TF = ThinkingFamily;
    // Qwen 3.x hybrid -- including the publisher-prefixed catalog form.
    REQUIRE(classify_thinking_family("qwen3.6-27b")              == TF::Qwen3Hybrid);
    REQUIRE(classify_thinking_family("Qwen/Qwen3-14B")           == TF::Qwen3Hybrid);
    REQUIRE(classify_thinking_family("qwen3-thinking-32b")       == TF::Qwen3Hybrid);
    // Qwen 2.x / legacy.
    REQUIRE(classify_thinking_family("qwen2.5-7b-instruct")      == TF::Qwen2Legacy);
    REQUIRE(classify_thinking_family("qwen-7b-chat")            == TF::Qwen2Legacy);
    // o1 / DeepSeek-R1 -- reasoning_effort. R1 wins over the embedded "qwen".
    REQUIRE(classify_thinking_family("o1-preview")              == TF::ReasoningEffort);
    REQUIRE(classify_thinking_family("deepseek-r1-distill-qwen-7b") == TF::ReasoningEffort);
    // Unknown.
    REQUIRE(classify_thinking_family("gemma-4-e4b")            == TF::Unknown);
    REQUIRE(classify_thinking_family("llama-3.1-8b-instruct")  == TF::Unknown);
    REQUIRE(classify_thinking_family("")                      == TF::Unknown);
}

TEST_CASE("Qwen 3.x off sets chat_template_kwargs.enable_thinking=false",
          "[s6.14][thinking]")
{
    auto msgs = sys_and_user("Fix the build.");
    json req = make_request(msgs);

    apply_thinking_mode(req, msgs, ThinkingMode::Off, "qwen/qwen3.6-27b");

    REQUIRE(req.contains("chat_template_kwargs"));
    REQUIRE(req["chat_template_kwargs"].contains("enable_thinking"));
    REQUIRE(req["chat_template_kwargs"]["enable_thinking"] == false);
    // The message content must NOT be touched for the hybrid path.
    REQUIRE(req["messages"].back()["content"] == "Fix the build.");
}

TEST_CASE("Qwen 3.x on sets chat_template_kwargs.enable_thinking=true",
          "[s6.14][thinking]")
{
    auto msgs = sys_and_user("Fix the build.");
    json req = make_request(msgs);

    apply_thinking_mode(req, msgs, ThinkingMode::On, "qwen3-14b");

    REQUIRE(req["chat_template_kwargs"]["enable_thinking"] == true);
}

TEST_CASE("Qwen 2.x off appends /no_think to the last user message",
          "[s6.14][thinking]")
{
    auto msgs = sys_and_user("Fix the build.");
    json req = make_request(msgs);

    apply_thinking_mode(req, msgs, ThinkingMode::Off, "qwen2.5-7b-instruct");

    // The in-vector message and the re-published request copy both carry it.
    REQUIRE(msgs.back().content == "Fix the build.\n/no_think");
    REQUIRE(req["messages"].back()["content"] == "Fix the build.\n/no_think");
    // System message is untouched.
    REQUIRE(req["messages"].front()["content"] == "You are Locus.");
    // No top-level template kwargs for the legacy path.
    REQUIRE_FALSE(req.contains("chat_template_kwargs"));
}

TEST_CASE("Qwen 2.x on appends /think to the last user message",
          "[s6.14][thinking]")
{
    auto msgs = sys_and_user("Fix the build.");
    json req = make_request(msgs);

    apply_thinking_mode(req, msgs, ThinkingMode::On, "qwen-7b-chat");

    REQUIRE(req["messages"].back()["content"] == "Fix the build.\n/think");
}

TEST_CASE("o1 / DeepSeek-R1 off sets reasoning_effort=low", "[s6.14][thinking]")
{
    auto msgs = sys_and_user("Fix the build.");
    json req = make_request(msgs);

    apply_thinking_mode(req, msgs, ThinkingMode::Off, "deepseek-r1-distill-qwen-7b");

    REQUIRE(req.contains("reasoning_effort"));
    REQUIRE(req["reasoning_effort"] == "low");
    REQUIRE(req["messages"].back()["content"] == "Fix the build.");
}

TEST_CASE("o1 on sets reasoning_effort=high", "[s6.14][thinking]")
{
    auto msgs = sys_and_user("Fix the build.");
    json req = make_request(msgs);

    apply_thinking_mode(req, msgs, ThinkingMode::On, "o1-preview");

    REQUIRE(req["reasoning_effort"] == "high");
}

TEST_CASE("Auto never augments the request (round-trip equality on any model)",
          "[s6.14][thinking]")
{
    for (const char* model : {"qwen3.6-27b", "qwen2.5-7b", "o1-preview",
                              "deepseek-r1-distill", "gemma-4-e4b", ""}) {
        auto msgs = sys_and_user("Fix the build.");
        json before = make_request(msgs);
        json after  = before;  // copy

        apply_thinking_mode(after, msgs, ThinkingMode::Auto, model);

        REQUIRE(after == before);
        // The message vector is untouched too.
        REQUIRE(msgs.back().content == "Fix the build.");
    }
}

TEST_CASE("Unknown model + non-Auto leaves the request unchanged and warns",
          "[s6.14][thinking]")
{
    // Capture warns via a ringbuffer sink installed on a temporary default
    // logger. The restore MUST run even if a REQUIRE below throws -- a leaked
    // capture logger would divert every subsequent test's log output into this
    // 64-entry ring. RAII scope guard, not a trailing statement, so the
    // restore survives the throwing path.
    auto ring = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(64);
    auto capture = std::make_shared<spdlog::logger>("thinking_test_capture", ring);
    capture->set_level(spdlog::level::trace);

    struct DefaultLoggerGuard {
        std::shared_ptr<spdlog::logger> prev;
        explicit DefaultLoggerGuard(std::shared_ptr<spdlog::logger> next)
            : prev(spdlog::default_logger())
        {
            spdlog::set_default_logger(std::move(next));
        }
        ~DefaultLoggerGuard() { spdlog::set_default_logger(prev); }
    } guard(capture);

    // A model id no other test in this binary feeds the helper, so the
    // once-per-process warn dedup can't have already swallowed it.
    const char* kUnknown = "mystery-model-s614-unique-xyz";

    auto msgs = sys_and_user("Fix the build.");
    json before = make_request(msgs);
    json after  = before;

    apply_thinking_mode(after, msgs, ThinkingMode::Off, kUnknown);

    // Request unchanged.
    REQUIRE(after == before);
    REQUIRE(msgs.back().content == "Fix the build.");

    // A warn naming the model + "no known mechanism" landed.
    bool warned = false;
    for (const auto& line : ring->last_formatted()) {
        if (line.find("no known mechanism") != std::string::npos &&
            line.find(kUnknown) != std::string::npos) {
            warned = true;
            break;
        }
    }

    REQUIRE(warned);
}

TEST_CASE("ThinkingMode string round-trip", "[s6.14][thinking]")
{
    REQUIRE(thinking_mode_from_string("auto") == ThinkingMode::Auto);
    REQUIRE(thinking_mode_from_string("on")   == ThinkingMode::On);
    REQUIRE(thinking_mode_from_string("off")  == ThinkingMode::Off);
    // Tolerant aliases.
    REQUIRE(thinking_mode_from_string("true")  == ThinkingMode::On);
    REQUIRE(thinking_mode_from_string("false") == ThinkingMode::Off);
    REQUIRE(thinking_mode_from_string("ON")    == ThinkingMode::On);
    // Unknown -> Auto (server default).
    REQUIRE(thinking_mode_from_string("garbage") == ThinkingMode::Auto);

    REQUIRE(std::string(to_string(ThinkingMode::Auto)) == "auto");
    REQUIRE(std::string(to_string(ThinkingMode::On))   == "on");
    REQUIRE(std::string(to_string(ThinkingMode::Off))  == "off");
}
