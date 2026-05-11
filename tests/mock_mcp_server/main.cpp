// Minimal MCP server used by the locus_tests suite. Speaks newline-delimited
// JSON-RPC 2.0 over stdin/stdout. Implements the bare subset Locus exercises:
//
//   initialize        -> { protocolVersion, capabilities, serverInfo }
//   tools/list        -> { tools: [echo, fail_once] }
//   tools/call/echo   -> { content: [{ type:"text", text: arguments.text }] }
//   tools/call/fail_once -> first call returns isError, second succeeds.
//                            (used to exercise per-call error paths.)
//
// Non-zero exit on the magic env var LOCUS_MOCK_MCP_CRASH_AFTER=<n>: the
// server processes <n> requests, then exits with code 1 to simulate a crash.
//
// Kept dependency-free except for nlohmann/json which the locus build
// already ships.

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using json = nlohmann::json;

namespace {

int crash_after = 0;
int requests_handled = 0;
int fail_once_calls = 0;

void send(const json& msg)
{
    std::string s = msg.dump();
    s.push_back('\n');
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

json handle_initialize(const json& /*params*/)
{
    json result;
    result["protocolVersion"] = "2025-06-18";
    result["capabilities"]    = { {"tools", json::object()} };
    result["serverInfo"]      = { {"name", "locus-mock-mcp-server"}, {"version", "0.1"} };
    return result;
}

json handle_tools_list()
{
    json echo_schema;
    echo_schema["type"] = "object";
    echo_schema["properties"] = {
        { "text", { {"type", "string"}, {"description", "Text to echo back"} } }
    };
    echo_schema["required"] = json::array({"text"});

    json fail_once_schema;
    fail_once_schema["type"] = "object";
    fail_once_schema["properties"] = json::object();

    json tools = json::array();
    tools.push_back({
        {"name", "echo"},
        {"description", "Echo the supplied text back to the caller."},
        {"inputSchema", echo_schema}
    });
    tools.push_back({
        {"name", "fail_once"},
        {"description", "Returns isError on the first call; succeeds afterwards."},
        {"inputSchema", fail_once_schema}
    });
    return { {"tools", tools} };
}

json handle_tools_call(const json& params)
{
    std::string name = params.value("name", std::string{});
    json args = params.value("arguments", json::object());

    json result;
    if (name == "echo") {
        std::string text = args.value("text", std::string{});
        result["content"] = json::array({
            { {"type", "text"}, {"text", text} }
        });
        result["isError"] = false;
        return result;
    }
    if (name == "fail_once") {
        ++fail_once_calls;
        if (fail_once_calls == 1) {
            result["content"] = json::array({
                { {"type", "text"}, {"text", "first call always fails"} }
            });
            result["isError"] = true;
        } else {
            result["content"] = json::array({
                { {"type", "text"}, {"text", "ok"} }
            });
            result["isError"] = false;
        }
        return result;
    }

    // Unknown tool: signal isError so the client surfaces it as a tool error
    // (vs a JSON-RPC protocol error, which the spec reserves for transport
    // and dispatch failures).
    result["content"] = json::array({
        { {"type", "text"}, {"text", "Unknown tool: " + name} }
    });
    result["isError"] = true;
    return result;
}

} // namespace

int main()
{
#ifdef _WIN32
    // Force binary mode so Windows doesn't translate \n to \r\n on stdout
    // (which would split the JSON line in unexpected places client-side).
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin),  _O_BINARY);
#endif

    if (const char* env = std::getenv("LOCUS_MOCK_MCP_CRASH_AFTER")) {
        crash_after = std::atoi(env);
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        json req;
        try {
            req = json::parse(line);
        } catch (...) {
            // Malformed: ignore -- the spec calls for an error response, but
            // we're a test-side mock and don't have an id to bind to.
            continue;
        }

        if (!req.contains("method") || !req["method"].is_string()) continue;
        std::string method = req["method"].get<std::string>();

        // Notifications: no id, no response.
        if (!req.contains("id")) continue;

        json id = req["id"];
        json params = req.value("params", json::object());

        json reply;
        reply["jsonrpc"] = "2.0";
        reply["id"]      = id;

        try {
            if (method == "initialize") {
                reply["result"] = handle_initialize(params);
            } else if (method == "tools/list") {
                reply["result"] = handle_tools_list();
            } else if (method == "tools/call") {
                reply["result"] = handle_tools_call(params);
            } else {
                reply["error"] = {
                    {"code", -32601},
                    {"message", "method not found: " + method}
                };
            }
        } catch (const std::exception& e) {
            reply["error"] = {
                {"code", -32603},
                {"message", std::string("internal error: ") + e.what()}
            };
        }

        send(reply);

        ++requests_handled;
        if (crash_after > 0 && requests_handled >= crash_after) {
            // Simulate a crash by exiting non-zero with no further response.
            std::_Exit(1);
        }
    }
    return 0;
}
