// S6.10 Task A -- JSON repair pre-pass unit tests.
//
// One Catch2 case per repair stage with a representative broken-input ->
// expected-output pair, plus the clean-input fast path and the end-to-end
// case threaded through the XmlToolCallExtractor.

#include "llm/json_repair.h"
#include "llm/xml_tool_call_extractor.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

using locus::repair_for_parse;
using nlohmann::json;

// Convenience: ask the helper to repair, then parse the result. Catch2 messages
// surface the diagnostic list when an assertion fails.
json repair_and_parse(const std::string& input,
                      std::string* diag_out = nullptr)
{
    auto r = repair_for_parse(input);
    REQUIRE(r.has_value());
    if (diag_out) *diag_out = r->stages_applied;
    return json::parse(r->fixed);
}

}  // namespace

TEST_CASE("json_repair: clean input round-trips unchanged",
          "[s6.10][json_repair]")
{
    auto r = repair_for_parse(R"({"path":"src/x.cpp","count":3})");
    REQUIRE(r.has_value());
    REQUIRE(r->stages_applied.empty());
    REQUIRE(r->fixed == R"({"path":"src/x.cpp","count":3})");
}

TEST_CASE("json_repair: stage 1 -- strip trailing commas",
          "[s6.10][json_repair]")
{
    std::string diag;
    json j = repair_and_parse(R"({"path":"src/x.cpp","count":3,})", &diag);
    REQUIRE(diag.find("trailing_comma") != std::string::npos);
    REQUIRE(j["path"] == "src/x.cpp");
    REQUIRE(j["count"] == 3);

    // Multiple trailing commas across object + array.
    json j2 = repair_and_parse(R"({"a":[1,2,3,],"b":"x",})");
    REQUIRE(j2["a"].size() == 3);
    REQUIRE(j2["b"] == "x");
}

TEST_CASE("json_repair: stage 1 leaves commas inside strings alone",
          "[s6.10][json_repair]")
{
    // `,}` inside a string is not a trailing comma -- the helper must not
    // touch it.
    auto r = repair_for_parse(R"({"msg":"items: a, b, c,","trailing":true})");
    REQUIRE(r.has_value());
    REQUIRE(r->stages_applied.empty());  // input was already valid
}

TEST_CASE("json_repair: stage 2 -- wrap unquoted identifier keys",
          "[s6.10][json_repair]")
{
    std::string diag;
    json j = repair_and_parse(R"({path: "src/x.cpp", count: 3})", &diag);
    REQUIRE(diag.find("unquoted_keys") != std::string::npos);
    REQUIRE(j["path"] == "src/x.cpp");
    REQUIRE(j["count"] == 3);

    // Mixed: one quoted, one unquoted.
    json j2 = repair_and_parse(R"({"a": 1, b: 2, c-d: 3})");
    REQUIRE(j2["a"] == 1);
    REQUIRE(j2["b"] == 2);
    REQUIRE(j2["c-d"] == 3);
}

TEST_CASE("json_repair: stage 3 -- single quotes to double quotes",
          "[s6.10][json_repair]")
{
    std::string diag;
    json j = repair_and_parse(R"({'path': 'src/x.cpp', 'count': 3})", &diag);
    REQUIRE(diag.find("single_quotes") != std::string::npos);
    REQUIRE(j["path"] == "src/x.cpp");
    REQUIRE(j["count"] == 3);
}

TEST_CASE("json_repair: stage 3 -- skipped when double quotes already present",
          "[s6.10][json_repair]")
{
    // Don't rewrite the apostrophe inside a string value -- that would corrupt
    // "don't". The helper detects the presence of any double quote and bails
    // on stage 3 entirely.
    auto r = repair_for_parse(R"({"msg":"don't break this"})");
    REQUIRE(r.has_value());
    REQUIRE(r->stages_applied.find("single_quotes") == std::string::npos);
    json j = json::parse(r->fixed);
    REQUIRE(j["msg"] == "don't break this");
}

TEST_CASE("json_repair: stage 4 -- patch missing closing braces / brackets",
          "[s6.10][json_repair]")
{
    std::string diag;
    json j = repair_and_parse(R"({"path":"src/x.cpp","items":[1,2,3)", &diag);
    REQUIRE(diag.find("balance") != std::string::npos);
    REQUIRE(j["path"] == "src/x.cpp");
    REQUIRE(j["items"].size() == 3);

    // Just the outer `}` missing.
    json j2 = repair_and_parse(R"({"path":"x","count":3)");
    REQUIRE(j2["count"] == 3);

    // Multiple missing closers across nested objects.
    json j3 = repair_and_parse(R"({"a":{"b":{"c":1)");
    REQUIRE(j3["a"]["b"]["c"] == 1);
}

TEST_CASE("json_repair: stage 4 -- close an unterminated string",
          "[s6.10][json_repair]")
{
    std::string diag;
    json j = repair_and_parse(R"({"path":"src/x.cpp)", &diag);
    REQUIRE(diag.find("balance") != std::string::npos);
    REQUIRE(j["path"] == "src/x.cpp");
}

TEST_CASE("json_repair: stage 5 -- escape literal newline / tab in string",
          "[s6.10][json_repair]")
{
    std::string diag;
    std::string raw = std::string(R"({"code":"line1)") + "\n" +
                      "line2\twith tab\"}";
    json j = repair_and_parse(raw, &diag);
    REQUIRE(diag.find("literal_controls") != std::string::npos);
    REQUIRE(j["code"] == "line1\nline2\twith tab");
}

TEST_CASE("json_repair: stage 6 -- extract balanced object from prose",
          "[s6.10][json_repair]")
{
    std::string diag;
    json j = repair_and_parse(
        R"(Sure! Here's the call: {"name":"x","args":{"k":1}} hope that helps)",
        &diag);
    REQUIRE(diag.find("extract_balanced") != std::string::npos);
    REQUIRE(j["name"] == "x");
    REQUIRE(j["args"]["k"] == 1);
}

TEST_CASE("json_repair: returns nullopt when repair cannot fix the input",
          "[s6.10][json_repair]")
{
    // Pathological garbage with no recognisable JSON structure.
    REQUIRE_FALSE(repair_for_parse("").has_value());
    REQUIRE_FALSE(repair_for_parse("totally not JSON at all").has_value());
    REQUIRE_FALSE(repair_for_parse(":}{:}{:").has_value());
}

TEST_CASE("json_repair: combined errors -- trailing comma + unquoted key",
          "[s6.10][json_repair]")
{
    // The canonical "two-error" case from the stage doc's end-to-end test.
    std::string diag;
    json j = repair_and_parse(R"({path: "src/x.cpp", count: 3,})", &diag);
    REQUIRE(diag.find("trailing_comma") != std::string::npos);
    REQUIRE(diag.find("unquoted_keys") != std::string::npos);
    REQUIRE(j["path"] == "src/x.cpp");
    REQUIRE(j["count"] == 3);
}

// End-to-end: the Qwen-XML decoder's body-parse error path runs through
// `repair_for_parse` and successfully dispatches a tool call.
TEST_CASE("json_repair: XmlToolCallExtractor recovers a malformed Qwen body",
          "[s6.10][json_repair][xml_extractor]")
{
    using locus::StreamDecoderSink;
    using locus::XmlMarker;
    using locus::XmlToolCallExtractor;

    XmlToolCallExtractor ext({XmlMarker::Qwen});

    std::string text_out;
    std::vector<StreamDecoderSink::ToolCallDelta> calls;

    auto on_text = [&](const std::string& t) { text_out += t; };
    auto on_call = [&](const StreamDecoderSink::ToolCallDelta& d) {
        calls.push_back(d);
    };

    // Trailing comma + unquoted key -- both stages 1 and 2 must fire.
    std::string chunk =
        "<tool_call>{name: \"read_file\", arguments: {path: \"src/x.cpp\",},}</tool_call>";
    ext.feed(chunk, on_text, on_call);
    ext.finish(on_text);

    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].name_frag == "read_file");

    json args = json::parse(calls[0].args_frag);
    REQUIRE(args["path"] == "src/x.cpp");
}
