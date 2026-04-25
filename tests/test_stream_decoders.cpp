#include <catch2/catch_test_macros.hpp>

#include "llm/llm_client.h"
#include "llm/stream_decoder.h"
#include "llm/xml_tool_call_extractor.h"
#include "llm/stream_decoders/openai_decoder.h"
#include "llm/stream_decoders/qwen_xml_decoder.h"
#include "llm/stream_decoders/claude_xml_decoder.h"
#include "llm/stream_decoders/auto_decoder.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace locus;
using json = nlohmann::json;

// ============================================================================
// Test helpers
// ============================================================================

namespace {

// Captures every event a decoder emits across one or more decode() calls
// so each test can assert on the materialised stream.
struct Capture {
    std::string                                       text;
    std::string                                       reasoning;
    std::vector<StreamDecoderSink::ToolCallDelta>     tool_calls;
    bool                                              had_usage = false;
    CompletionUsage                                   usage{};

    StreamDecoderSink make_sink()
    {
        StreamDecoderSink s;
        s.on_text = [this](const std::string& t) { text += t; };
        s.on_reasoning = [this](const std::string& t) { reasoning += t; };
        s.on_tool_call_delta = [this](const StreamDecoderSink::ToolCallDelta& d) {
            // Coalesce by index, mimicking what LMStudioClient does.
            while (static_cast<int>(tool_calls.size()) <= d.index)
                tool_calls.push_back({});
            auto& a = tool_calls[d.index];
            a.index = d.index;
            if (!d.id_frag.empty())   a.id_frag = d.id_frag;
            a.name_frag += d.name_frag;
            a.args_frag += d.args_frag;
        };
        s.on_usage = [this](const CompletionUsage& u) {
            had_usage = true;
            usage = u;
        };
        return s;
    }
};

// Wrap a content fragment in an OpenAI SSE chunk shell. The decoder
// stack sees the same shape regardless of which dialect's body it is.
std::string content_chunk(const std::string& content)
{
    json j = {
        {"choices", json::array({
            json::object({
                {"index", 0},
                {"delta", json::object({{"content", content}})}
            })
        })}
    };
    return j.dump();
}

std::string usage_chunk(int prompt, int completion, int total)
{
    json j = {
        {"choices", json::array({
            json::object({
                {"index", 0},
                {"delta", json::object()}
            })
        })},
        {"usage", json::object({
            {"prompt_tokens", prompt},
            {"completion_tokens", completion},
            {"total_tokens", total}
        })}
    };
    return j.dump();
}

} // namespace

// ============================================================================
// ToolFormat string round-trip
// ============================================================================

TEST_CASE("ToolFormat: parse known names and aliases", "[s4.n][tool-format]")
{
    CHECK(tool_format_from_string("auto")     == ToolFormat::Auto);
    CHECK(tool_format_from_string("openai")   == ToolFormat::OpenAi);
    CHECK(tool_format_from_string("OpenAI")   == ToolFormat::OpenAi);
    CHECK(tool_format_from_string("open-ai")  == ToolFormat::OpenAi);
    CHECK(tool_format_from_string("qwen")     == ToolFormat::Qwen);
    CHECK(tool_format_from_string("Claude")   == ToolFormat::Claude);
    CHECK(tool_format_from_string("xml")      == ToolFormat::Claude);
    CHECK(tool_format_from_string("none")     == ToolFormat::None);
    CHECK(tool_format_from_string("off")      == ToolFormat::None);
    // Unknown values fall back to Auto.
    CHECK(tool_format_from_string("garbage")  == ToolFormat::Auto);
    CHECK(tool_format_from_string("")         == ToolFormat::Auto);
}

TEST_CASE("ToolFormat: round-trip via to_string", "[s4.n][tool-format]")
{
    for (auto f : {ToolFormat::Auto, ToolFormat::OpenAi, ToolFormat::Qwen,
                   ToolFormat::Claude, ToolFormat::None}) {
        auto round = tool_format_from_string(to_string(f));
        CHECK(round == f);
    }
}

// ============================================================================
// OpenAI decoder still works (regression baseline)
// ============================================================================

TEST_CASE("OpenAiDecoder: text content passes through", "[s4.n][decoders]")
{
    OpenAiDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    d.decode(content_chunk("hello "), sink);
    d.decode(content_chunk("world"),  sink);
    CHECK(cap.text == "hello world");
    CHECK(cap.tool_calls.empty());
}

TEST_CASE("OpenAiDecoder: native tool_calls deltas surface", "[s4.n][decoders]")
{
    OpenAiDecoder d;
    Capture cap;
    auto sink = cap.make_sink();

    std::string chunk = R"({"choices":[{"index":0,"delta":{"tool_calls":[)"
                        R"({"index":0,"id":"call_1","function":{)"
                        R"("name":"read_file","arguments":"{\"path\":\"a\"}")"
                        R"(}}]}}]})";
    d.decode(chunk, sink);
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].id_frag == "call_1");
    CHECK(cap.tool_calls[0].name_frag == "read_file");
    CHECK(cap.tool_calls[0].args_frag == R"({"path":"a"})");
}

TEST_CASE("OpenAiDecoder: usage propagates", "[s4.n][decoders]")
{
    OpenAiDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    d.decode(usage_chunk(100, 50, 150), sink);
    CHECK(cap.had_usage);
    CHECK(cap.usage.prompt_tokens == 100);
    CHECK(cap.usage.completion_tokens == 50);
    CHECK(cap.usage.total_tokens == 150);
}

// ============================================================================
// Qwen decoder
// ============================================================================

TEST_CASE("QwenXmlDecoder: single embedded tool call, single chunk",
          "[s4.n][decoders][qwen]")
{
    QwenXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();

    std::string body =
        "Here we go. <tool_call>"
        R"({"name":"read_file","arguments":{"path":"src/main.cpp"}})"
        "</tool_call> done.";
    d.decode(content_chunk(body), sink);
    d.finish_stream(sink);

    CHECK(cap.text == "Here we go.  done.");
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].name_frag == "read_file");
    CHECK(cap.tool_calls[0].args_frag == R"({"path":"src/main.cpp"})");
    CHECK(!cap.tool_calls[0].id_frag.empty());
}

TEST_CASE("QwenXmlDecoder: arguments-as-string variant",
          "[s4.n][decoders][qwen]")
{
    QwenXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    std::string body =
        "<tool_call>"
        R"({"name":"search","arguments":"{\"query\":\"foo\"}"})"
        "</tool_call>";
    d.decode(content_chunk(body), sink);
    d.finish_stream(sink);

    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].name_frag == "search");
    CHECK(cap.tool_calls[0].args_frag == R"({"query":"foo"})");
}

TEST_CASE("QwenXmlDecoder: tool call split across SSE chunks",
          "[s4.n][decoders][qwen]")
{
    QwenXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();

    // Splice the body into pieces that cut through:
    //   - the opening marker
    //   - the JSON
    //   - the closing marker
    d.decode(content_chunk("Pre <to"),                      sink);
    d.decode(content_chunk("ol_call>"),                     sink);
    d.decode(content_chunk(R"({"name":"x","arguments")"),   sink);
    d.decode(content_chunk(R"(:{"k":1}})"),                 sink);
    d.decode(content_chunk("</tool_"),                      sink);
    d.decode(content_chunk("call> Post"),                   sink);
    d.finish_stream(sink);

    CHECK(cap.text == "Pre  Post");
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].name_frag == "x");
    CHECK(cap.tool_calls[0].args_frag == R"({"k":1})");
}

TEST_CASE("QwenXmlDecoder: two tool calls back-to-back",
          "[s4.n][decoders][qwen]")
{
    QwenXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();

    std::string body =
        "<tool_call>"
        R"({"name":"a","arguments":{}})"
        "</tool_call>"
        "and "
        "<tool_call>"
        R"({"name":"b","arguments":{"q":"hi"}})"
        "</tool_call>";
    d.decode(content_chunk(body), sink);
    d.finish_stream(sink);

    CHECK(cap.text == "and ");
    REQUIRE(cap.tool_calls.size() == 2);
    CHECK(cap.tool_calls[0].name_frag == "a");
    CHECK(cap.tool_calls[1].name_frag == "b");
    CHECK(cap.tool_calls[1].args_frag == R"({"q":"hi"})");
    CHECK(cap.tool_calls[0].id_frag != cap.tool_calls[1].id_frag);
}

TEST_CASE("QwenXmlDecoder: malformed JSON inside marker is dropped, no crash",
          "[s4.n][decoders][qwen]")
{
    QwenXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    d.decode(content_chunk("ok <tool_call>not json</tool_call> after"), sink);
    d.finish_stream(sink);

    // The malformed tool call is dropped; prefix + suffix text passes
    // through (the inner body is consumed because we did see the close
    // marker -- only the parsing failed).
    CHECK(cap.tool_calls.empty());
    CHECK(cap.text == "ok  after");
}

TEST_CASE("QwenXmlDecoder: native OpenAI tool_calls also pass through",
          "[s4.n][decoders][qwen]")
{
    QwenXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    std::string chunk = R"({"choices":[{"index":0,"delta":{"tool_calls":[)"
                        R"({"index":0,"id":"native_1","function":{)"
                        R"("name":"x","arguments":"{}"}}]}}]})";
    d.decode(chunk, sink);
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].id_frag == "native_1");
    CHECK(cap.tool_calls[0].name_frag == "x");
}

TEST_CASE("QwenXmlDecoder: text without markers is unchanged",
          "[s4.n][decoders][qwen]")
{
    QwenXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    d.decode(content_chunk("plain text "), sink);
    d.decode(content_chunk("with <angle> tags"), sink);
    d.finish_stream(sink);
    CHECK(cap.text == "plain text with <angle> tags");
    CHECK(cap.tool_calls.empty());
}

// ============================================================================
// Claude decoder
// ============================================================================

TEST_CASE("ClaudeXmlDecoder: single invoke, single parameter",
          "[s4.n][decoders][claude]")
{
    ClaudeXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();

    std::string body =
        "Pre <function_calls>"
        "<invoke name=\"read_file\">"
        "<parameter name=\"path\">src/main.cpp</parameter>"
        "</invoke>"
        "</function_calls> Post";
    d.decode(content_chunk(body), sink);
    d.finish_stream(sink);

    CHECK(cap.text == "Pre  Post");
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].name_frag == "read_file");

    auto args = json::parse(cap.tool_calls[0].args_frag);
    CHECK(args["path"] == "src/main.cpp");
}

TEST_CASE("ClaudeXmlDecoder: typed parameter values",
          "[s4.n][decoders][claude]")
{
    ClaudeXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();

    std::string body =
        "<function_calls>"
        "<invoke name=\"page\">"
        "<parameter name=\"path\">x.txt</parameter>"
        "<parameter name=\"offset\">42</parameter>"
        "<parameter name=\"recursive\">true</parameter>"
        "<parameter name=\"globs\">[\"*.cpp\",\"*.h\"]</parameter>"
        "</invoke>"
        "</function_calls>";
    d.decode(content_chunk(body), sink);
    d.finish_stream(sink);

    REQUIRE(cap.tool_calls.size() == 1);
    auto args = json::parse(cap.tool_calls[0].args_frag);
    CHECK(args["path"]      == "x.txt");
    CHECK(args["offset"]    == 42);
    CHECK(args["recursive"] == true);
    CHECK(args["globs"].is_array());
    CHECK(args["globs"][0]  == "*.cpp");
}

TEST_CASE("ClaudeXmlDecoder: multiple invokes inside one function_calls",
          "[s4.n][decoders][claude]")
{
    ClaudeXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();

    std::string body =
        "<function_calls>"
        "<invoke name=\"a\"><parameter name=\"x\">1</parameter></invoke>"
        "<invoke name=\"b\"><parameter name=\"y\">2</parameter></invoke>"
        "</function_calls>";
    d.decode(content_chunk(body), sink);
    d.finish_stream(sink);

    REQUIRE(cap.tool_calls.size() == 2);
    CHECK(cap.tool_calls[0].name_frag == "a");
    CHECK(cap.tool_calls[1].name_frag == "b");
    CHECK(cap.tool_calls[0].id_frag != cap.tool_calls[1].id_frag);
}

TEST_CASE("ClaudeXmlDecoder: parameter body decodes XML entities",
          "[s4.n][decoders][claude]")
{
    ClaudeXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();

    std::string body =
        "<function_calls>"
        "<invoke name=\"echo\">"
        "<parameter name=\"msg\">a &amp; b &lt; c</parameter>"
        "</invoke>"
        "</function_calls>";
    d.decode(content_chunk(body), sink);
    d.finish_stream(sink);

    REQUIRE(cap.tool_calls.size() == 1);
    auto args = json::parse(cap.tool_calls[0].args_frag);
    CHECK(args["msg"] == "a & b < c");
}

TEST_CASE("ClaudeXmlDecoder: split-across-chunks marker still detected",
          "[s4.n][decoders][claude]")
{
    ClaudeXmlDecoder d;
    Capture cap;
    auto sink = cap.make_sink();

    d.decode(content_chunk("X <function"),                 sink);
    d.decode(content_chunk("_calls><invoke name=\"f\">"),  sink);
    d.decode(content_chunk("<parameter name=\"a\">v</parameter>"), sink);
    d.decode(content_chunk("</invoke></function_calls>Y"), sink);
    d.finish_stream(sink);

    CHECK(cap.text == "X Y");
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].name_frag == "f");
}

// ============================================================================
// Auto decoder
// ============================================================================

TEST_CASE("AutoToolFormatDecoder: detects Qwen marker",
          "[s4.n][decoders][auto]")
{
    AutoToolFormatDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    d.decode(content_chunk(
        "<tool_call>{\"name\":\"q\",\"arguments\":{}}</tool_call>"), sink);
    d.finish_stream(sink);
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].name_frag == "q");
}

TEST_CASE("AutoToolFormatDecoder: detects Claude marker",
          "[s4.n][decoders][auto]")
{
    AutoToolFormatDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    d.decode(content_chunk(
        "<function_calls><invoke name=\"c\">"
        "<parameter name=\"a\">1</parameter></invoke></function_calls>"), sink);
    d.finish_stream(sink);
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].name_frag == "c");
}

TEST_CASE("AutoToolFormatDecoder: prefers earliest marker when both present",
          "[s4.n][decoders][auto]")
{
    AutoToolFormatDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    // Qwen marker appears first; Claude tag is just text.
    d.decode(content_chunk(
        "<tool_call>{\"name\":\"q\",\"arguments\":{}}</tool_call> "
        "and <function_calls> something"), sink);
    d.finish_stream(sink);
    // Claude marker has no closing tag in the stream so finish flushes
    // the partial content as plain text. We only care that Qwen fired.
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].name_frag == "q");
}

TEST_CASE("AutoToolFormatDecoder: native OpenAI tool_calls still surface",
          "[s4.n][decoders][auto]")
{
    AutoToolFormatDecoder d;
    Capture cap;
    auto sink = cap.make_sink();
    std::string chunk = R"({"choices":[{"index":0,"delta":{"tool_calls":[)"
                        R"({"index":0,"id":"openai_1","function":{)"
                        R"("name":"x","arguments":"{}"}}]}}]})";
    d.decode(chunk, sink);
    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].id_frag == "openai_1");
}

// ============================================================================
// Reset between streams
// ============================================================================

TEST_CASE("Decoders: reset clears mid-stream state",
          "[s4.n][decoders]")
{
    QwenXmlDecoder d;
    {
        Capture cap;
        auto sink = cap.make_sink();
        // First stream truncates inside a tool call.
        d.decode(content_chunk("<tool_call>{\"name\":\"a\""), sink);
        d.finish_stream(sink);
    }
    // Reset; second stream fully closes a different call.
    d.reset();

    Capture cap;
    auto sink = cap.make_sink();
    d.decode(content_chunk(
        "<tool_call>{\"name\":\"b\",\"arguments\":{}}</tool_call>"), sink);
    d.finish_stream(sink);

    REQUIRE(cap.tool_calls.size() == 1);
    CHECK(cap.tool_calls[0].name_frag == "b");
    // Indices restart at 0 for the new stream.
    CHECK(cap.tool_calls[0].index == 0);
}

// ============================================================================
// Direct extractor unit tests (faster for edge cases)
// ============================================================================

TEST_CASE("XmlToolCallExtractor: holds back partial-marker suffix",
          "[s4.n][xml-extractor]")
{
    XmlToolCallExtractor x({XmlMarker::Qwen});
    std::string text;
    std::vector<StreamDecoderSink::ToolCallDelta> calls;
    auto on_t = [&](const std::string& s) { text += s; };
    auto on_c = [&](const StreamDecoderSink::ToolCallDelta& d) {
        calls.push_back(d);
    };

    // Feed text ending in a partial "<tool_call" -- must NOT be emitted
    // until we know whether it completes a real marker.
    x.feed("hi <tool_ca", on_t, on_c);
    CHECK(text == "hi ");        // suffix held back

    // Continuation completes the marker, then closes the call.
    x.feed("ll>{\"name\":\"q\",\"arguments\":{}}</tool_call> bye", on_t, on_c);
    x.finish(on_t);

    CHECK(text == "hi  bye");
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name_frag == "q");
}

TEST_CASE("XmlToolCallExtractor: emits truncated body as text on finish",
          "[s4.n][xml-extractor]")
{
    XmlToolCallExtractor x({XmlMarker::Qwen});
    std::string text;
    std::vector<StreamDecoderSink::ToolCallDelta> calls;
    auto on_t = [&](const std::string& s) { text += s; };
    auto on_c = [&](const StreamDecoderSink::ToolCallDelta& d) {
        calls.push_back(d);
    };

    x.feed("<tool_call>{\"name\":\"q\",\"args", on_t, on_c);
    x.finish(on_t);

    // Truncated mid-body: no tool call, but the partial body surfaces
    // as text so the user can see what came through.
    CHECK(calls.empty());
    CHECK(text.find("\"name\":\"q\"") != std::string::npos);
}
