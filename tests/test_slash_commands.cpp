#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "agent/slash_commands.h"
#include "tool.h"
#include "tool_registry.h"
#include "support/fake_workspace_services.h"

#include <filesystem>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;

namespace {

// A tool with one of each type so parser tests can exercise all coercions
// without pulling in the real tool set (which has file-system side effects).
class FakeTypedTool : public locus::ITool {
public:
    std::string name()        const override { return "echo"; }
    std::string description() const override { return "echo args back"; }

    std::vector<locus::ToolParam> params() const override {
        return {
            {"path",    "string",  "positional path",     true},
            {"offset",  "integer", "optional offset",     false},
            {"recurse", "boolean", "optional recursion",  false},
        };
    }

    locus::ToolResult execute(const locus::ToolCall& call,
                              locus::IWorkspaceServices&) override {
        locus::ToolResult r;
        r.success = true;
        r.content = call.args.dump();
        r.display = "echo: " + call.args.dump();
        return r;
    }
};

class NoArgTool : public locus::ITool {
public:
    std::string name()        const override { return "ping"; }
    std::string description() const override { return "no args"; }
    std::vector<locus::ToolParam> params() const override { return {}; }
    locus::ToolResult execute(const locus::ToolCall&,
                              locus::IWorkspaceServices&) override {
        return {true, "pong", "pong"};
    }
};

locus::ToolRegistry make_registry()
{
    locus::ToolRegistry reg;
    reg.register_tool(std::make_unique<FakeTypedTool>());
    reg.register_tool(std::make_unique<NoArgTool>());
    return reg;
}

} // namespace

// -- Parser ----------------------------------------------------------------

TEST_CASE("SlashCommandParser: returns nullopt for non-slash input", "[s3.j]")
{
    auto reg = make_registry();
    REQUIRE_FALSE(locus::SlashCommandParser::parse("hello",       reg).has_value());
    REQUIRE_FALSE(locus::SlashCommandParser::parse("",            reg).has_value());
    REQUIRE_FALSE(locus::SlashCommandParser::parse("not a slash", reg).has_value());
}

TEST_CASE("SlashCommandParser: bare / or whitespace-only returns nullopt", "[s3.j]")
{
    auto reg = make_registry();
    REQUIRE_FALSE(locus::SlashCommandParser::parse("/",      reg).has_value());
    REQUIRE_FALSE(locus::SlashCommandParser::parse("/  \t ", reg).has_value());
}

TEST_CASE("SlashCommandParser: /help sets is_help", "[s3.j]")
{
    auto reg = make_registry();
    auto r = locus::SlashCommandParser::parse("/help", reg);
    REQUIRE(r.has_value());
    REQUIRE(r->is_help);
}

TEST_CASE("SlashCommandParser: positional args map to params in order", "[s3.j]")
{
    auto reg = make_registry();
    auto r = locus::SlashCommandParser::parse("/echo src/main.cpp 42 true", reg);
    REQUIRE(r.has_value());
    REQUIRE(r->tool_name == "echo");
    REQUIRE(r->args["path"]    == "src/main.cpp");
    REQUIRE(r->args["offset"]  == 42);
    REQUIRE(r->args["recurse"] == true);
}

TEST_CASE("SlashCommandParser: key=value overrides positional mapping", "[s3.j]")
{
    auto reg = make_registry();
    auto r = locus::SlashCommandParser::parse("/echo offset=7 src/a.cpp", reg);
    REQUIRE(r.has_value());
    REQUIRE(r->args["offset"] == 7);
    REQUIRE(r->args["path"]   == "src/a.cpp");
}

TEST_CASE("SlashCommandParser: quoted args preserve spaces", "[s3.j]")
{
    auto reg = make_registry();
    auto r = locus::SlashCommandParser::parse(R"(/echo path="hello world" offset=3)", reg);
    REQUIRE(r.has_value());
    REQUIRE(r->args["path"]   == "hello world");
    REQUIRE(r->args["offset"] == 3);
}

TEST_CASE("SlashCommandParser: boolean coercion accepts common aliases", "[s3.j]")
{
    auto reg = make_registry();
    for (auto* s : { "/echo p recurse=true",
                     "/echo p recurse=1",
                     "/echo p recurse=yes",
                     "/echo p recurse=ON" }) {
        auto r = locus::SlashCommandParser::parse(s, reg);
        REQUIRE(r.has_value());
        REQUIRE(r->args["recurse"] == true);
    }
    for (auto* s : { "/echo p recurse=false",
                     "/echo p recurse=0",
                     "/echo p recurse=no",
                     "/echo p recurse=OFF" }) {
        auto r = locus::SlashCommandParser::parse(s, reg);
        REQUIRE(r.has_value());
        REQUIRE(r->args["recurse"] == false);
    }
}

TEST_CASE("SlashCommandParser: unknown tool still parses (dispatcher errors)", "[s3.j]")
{
    auto reg = make_registry();
    auto r = locus::SlashCommandParser::parse("/nonexistent foo bar", reg);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->is_help);
    REQUIRE(r->tool_name == "nonexistent");
}

TEST_CASE("SlashCommandParser: bad integer throws SlashParseError", "[s3.j]")
{
    auto reg = make_registry();
    REQUIRE_THROWS_AS(
        locus::SlashCommandParser::parse("/echo p offset=not-a-number", reg),
        locus::SlashParseError);
}

TEST_CASE("SlashCommandParser: bad boolean throws SlashParseError", "[s3.j]")
{
    auto reg = make_registry();
    REQUIRE_THROWS_AS(
        locus::SlashCommandParser::parse("/echo p recurse=maybe", reg),
        locus::SlashParseError);
}

TEST_CASE("SlashCommandParser: unterminated quote throws SlashParseError", "[s3.j]")
{
    auto reg = make_registry();
    REQUIRE_THROWS_AS(
        locus::SlashCommandParser::parse(R"(/echo path="oops)", reg),
        locus::SlashParseError);
}

// -- Dispatcher ------------------------------------------------------------

struct CaptureStreams {
    std::vector<std::string> out;
    std::vector<std::string> err;
    std::function<void(std::string)> on_out = [this](std::string s){ out.push_back(std::move(s)); };
    std::function<void(std::string)> on_err = [this](std::string s){ err.push_back(std::move(s)); };
};

TEST_CASE("SlashCommandDispatcher: returns false for non-slash input", "[s3.j]")
{
    auto reg = make_registry();
    locus::test::FakeWorkspaceServices ws{std::filesystem::temp_directory_path()};
    locus::SlashCommandDispatcher dispatcher(reg, ws);

    CaptureStreams caps;
    REQUIRE_FALSE(dispatcher.try_dispatch("plain text", caps.on_out, caps.on_err));
    REQUIRE(caps.out.empty());
    REQUIRE(caps.err.empty());
}

TEST_CASE("SlashCommandDispatcher: /help emits listing", "[s3.j]")
{
    auto reg = make_registry();
    locus::test::FakeWorkspaceServices ws{std::filesystem::temp_directory_path()};
    locus::SlashCommandDispatcher dispatcher(reg, ws);

    CaptureStreams caps;
    REQUIRE(dispatcher.try_dispatch("/help", caps.on_out, caps.on_err));
    REQUIRE(caps.err.empty());
    REQUIRE(caps.out.size() == 1);
    REQUIRE_THAT(caps.out[0], ContainsSubstring("/echo"));
    REQUIRE_THAT(caps.out[0], ContainsSubstring("/ping"));
    REQUIRE_THAT(caps.out[0], ContainsSubstring("/help"));
}

TEST_CASE("SlashCommandDispatcher: unknown command reports error", "[s3.j]")
{
    auto reg = make_registry();
    locus::test::FakeWorkspaceServices ws{std::filesystem::temp_directory_path()};
    locus::SlashCommandDispatcher dispatcher(reg, ws);

    CaptureStreams caps;
    REQUIRE(dispatcher.try_dispatch("/nonexistent", caps.on_out, caps.on_err));
    REQUIRE(caps.out.empty());
    REQUIRE(caps.err.size() == 1);
    REQUIRE_THAT(caps.err[0], ContainsSubstring("Unknown command"));
}

TEST_CASE("SlashCommandDispatcher: executes known tool and formats result", "[s3.j]")
{
    auto reg = make_registry();
    locus::test::FakeWorkspaceServices ws{std::filesystem::temp_directory_path()};
    locus::SlashCommandDispatcher dispatcher(reg, ws);

    CaptureStreams caps;
    REQUIRE(dispatcher.try_dispatch("/ping", caps.on_out, caps.on_err));
    REQUIRE(caps.err.empty());
    REQUIRE(caps.out.size() == 1);
    REQUIRE_THAT(caps.out[0], ContainsSubstring("**/ping**"));
    REQUIRE_THAT(caps.out[0], ContainsSubstring("OK"));
    REQUIRE_THAT(caps.out[0], ContainsSubstring("pong"));
}

TEST_CASE("SlashCommandDispatcher: malformed input reports error, not crash", "[s3.j]")
{
    auto reg = make_registry();
    locus::test::FakeWorkspaceServices ws{std::filesystem::temp_directory_path()};
    locus::SlashCommandDispatcher dispatcher(reg, ws);

    CaptureStreams caps;
    REQUIRE(dispatcher.try_dispatch("/echo path offset=oops",
                                    caps.on_out, caps.on_err));
    REQUIRE(caps.out.empty());
    REQUIRE(caps.err.size() == 1);
    REQUIRE_THAT(caps.err[0], ContainsSubstring("Slash command error"));
}

TEST_CASE("SlashCommandDispatcher: complete lists all tools for empty prefix", "[s3.j]")
{
    auto reg = make_registry();
    locus::test::FakeWorkspaceServices ws{std::filesystem::temp_directory_path()};
    locus::SlashCommandDispatcher dispatcher(reg, ws);

    auto c = dispatcher.complete("");
    REQUIRE(c.size() == 2);
    REQUIRE(c[0].name == "echo");
    REQUIRE_THAT(c[0].signature, ContainsSubstring("<path>"));
    REQUIRE_THAT(c[0].signature, ContainsSubstring("[offset=integer]"));
}

TEST_CASE("SlashCommandDispatcher: complete prefers prefix matches", "[s3.j]")
{
    auto reg = make_registry();
    locus::test::FakeWorkspaceServices ws{std::filesystem::temp_directory_path()};
    locus::SlashCommandDispatcher dispatcher(reg, ws);

    auto c = dispatcher.complete("ec");
    REQUIRE(c.size() == 1);
    REQUIRE(c[0].name == "echo");

    auto miss = dispatcher.complete("zzz");
    REQUIRE(miss.empty());
}
