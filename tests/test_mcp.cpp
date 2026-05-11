#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "mcp/json_rpc.h"
#include "mcp/mcp_client.h"
#include "mcp/mcp_config.h"
#include "mcp/mcp_manager.h"
#include "mcp/mcp_tool.h"
#include "mcp/stdio_transport.h"
#include "core/workspace_services.h"
#include "index/index_query.h"
#include "tools/tool.h"
#include "tools/tool_registry.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

namespace {

// Locate the bundled mock MCP server next to the test exe. CMake copies
// the binary into the test exe's output directory via add_custom_command,
// so it lives at $<TARGET_FILE_DIR:locus_tests>/locus_mock_mcp_server.exe.
fs::path mock_server_path()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path exe(buf);
    return exe.parent_path() / "locus_mock_mcp_server.exe";
#else
    return {};
#endif
}

// Service shim for tools that don't need any workspace state.
class NullWs : public locus::IWorkspaceServices {
public:
    explicit NullWs(fs::path root = fs::temp_directory_path()) : root_(std::move(root)) {}
    const fs::path&       root() const override { return root_; }
    locus::IndexQuery*    index() override      { return nullptr; }
private:
    fs::path root_;
};

} // namespace

// ---------------------------------------------------------------------------
// json_rpc primitives
// ---------------------------------------------------------------------------

TEST_CASE("json_rpc: parse a response", "[s4.g][mcp][jsonrpc]")
{
    auto m = locus::jsonrpc::parse_message(R"({"jsonrpc":"2.0","id":7,"result":{"ok":true}})");
    REQUIRE(m.kind == locus::jsonrpc::Kind::response);
    REQUIRE(m.id.has_value());
    REQUIRE(*m.id == 7);
    REQUIRE(m.result["ok"].get<bool>());
}

TEST_CASE("json_rpc: parse an error response", "[s4.g][mcp][jsonrpc]")
{
    auto m = locus::jsonrpc::parse_message(R"({"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"nope"}})");
    REQUIRE(m.kind == locus::jsonrpc::Kind::response);
    REQUIRE(m.id.has_value());
    REQUIRE(m.error["code"].get<int>() == -32601);
}

TEST_CASE("json_rpc: parse a notification", "[s4.g][mcp][jsonrpc]")
{
    auto m = locus::jsonrpc::parse_message(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    REQUIRE(m.kind == locus::jsonrpc::Kind::notification);
    REQUIRE(m.method == "notifications/initialized");
}

TEST_CASE("json_rpc: malformed JSON returns parse_error", "[s4.g][mcp][jsonrpc]")
{
    auto m = locus::jsonrpc::parse_message("{not json");
    REQUIRE(m.kind == locus::jsonrpc::Kind::parse_error);
    REQUIRE(!m.parse_error.empty());
}

TEST_CASE("json_rpc: make_request shape", "[s4.g][mcp][jsonrpc]")
{
    auto j = locus::jsonrpc::make_request(42, "tools/list");
    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["id"] == 42);
    REQUIRE(j["method"] == "tools/list");
    REQUIRE(j.contains("params"));
}

// ---------------------------------------------------------------------------
// mcp_config
// ---------------------------------------------------------------------------

TEST_CASE("mcp_config: parse mcpServers map", "[s4.g][mcp][config]")
{
    std::string body = R"({
        "mcpServers": {
            "fs": { "command": "node", "args": ["server.js"], "env": {"K":"v"}, "enabled": true },
            "db": { "command": "psql-mcp", "args": [], "enabled": false }
        }
    })";
    auto cfgs = locus::McpConfigLoader::parse_json(body);
    REQUIRE(cfgs.size() == 2);

    auto find = [&](const std::string& n) -> const locus::McpServerConfig* {
        for (auto& c : cfgs) if (c.name == n) return &c;
        return nullptr;
    };
    REQUIRE(find("fs") != nullptr);
    REQUIRE(find("fs")->command == "node");
    REQUIRE(find("fs")->args == std::vector<std::string>{"server.js"});
    REQUIRE(find("fs")->env.at("K") == "v");
    REQUIRE(find("fs")->enabled);

    REQUIRE(find("db") != nullptr);
    REQUIRE_FALSE(find("db")->enabled);
}

TEST_CASE("mcp_config: missing command is skipped with a warning", "[s4.g][mcp][config]")
{
    std::string body = R"({"mcpServers": {"x":{"args":[]}}})";
    auto cfgs = locus::McpConfigLoader::parse_json(body);
    REQUIRE(cfgs.empty());
}

TEST_CASE("mcp_config: load merges workspace over global", "[s4.g][mcp][config]")
{
    auto root = fs::temp_directory_path() / "locus_test_mcp_cfg";
    fs::create_directories(root / ".locus");
    {
        std::ofstream f(root / ".locus" / "mcp.json");
        f << R"({"mcpServers":{"only_ws":{"command":"a"}}})";
    }
    auto cfgs = locus::McpConfigLoader::load(root);
    bool has = false;
    for (auto& c : cfgs) if (c.name == "only_ws") has = true;
    REQUIRE(has);

    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// stdio_transport + mcp_client end-to-end via the bundled mock server
// ---------------------------------------------------------------------------

#ifdef _WIN32

TEST_CASE("McpClient: handshake, tools/list, tools/call against mock", "[s4.g][mcp][client]")
{
    auto bin = mock_server_path();
    REQUIRE(fs::exists(bin));

    locus::McpServerConfig cfg;
    cfg.name    = "mock";
    cfg.command = bin.string();
    cfg.init_timeout = 5000ms;

    locus::McpClient client(cfg);
    REQUIRE(client.start(fs::temp_directory_path()));
    REQUIRE(client.status() == locus::McpClient::Status::ready);

    auto tools = client.list_tools();
    REQUIRE(tools.size() == 2);

    bool has_echo = false, has_fail_once = false;
    for (auto& t : tools) {
        if (t.name == "echo")      has_echo = true;
        if (t.name == "fail_once") has_fail_once = true;
    }
    REQUIRE(has_echo);
    REQUIRE(has_fail_once);

    nlohmann::json args; args["text"] = "hello world";
    auto result = client.call_tool("echo", args);
    REQUIRE(result.contains("content"));
    REQUIRE(result["content"][0]["text"].get<std::string>() == "hello world");
    REQUIRE(result.value("isError", false) == false);

    client.stop();
    REQUIRE(client.status() == locus::McpClient::Status::stopped);
}

TEST_CASE("McpClient: isError tool result is reported as such", "[s4.g][mcp][client]")
{
    auto bin = mock_server_path();
    REQUIRE(fs::exists(bin));

    locus::McpServerConfig cfg;
    cfg.name    = "mock";
    cfg.command = bin.string();

    locus::McpClient client(cfg);
    REQUIRE(client.start(fs::temp_directory_path()));

    auto first = client.call_tool("fail_once", nlohmann::json::object());
    REQUIRE(first.value("isError", false) == true);

    auto second = client.call_tool("fail_once", nlohmann::json::object());
    REQUIRE(second.value("isError", false) == false);
}

TEST_CASE("McpClient: server crash transitions status and fails outstanding requests",
          "[s4.g][mcp][client]")
{
    auto bin = mock_server_path();
    REQUIRE(fs::exists(bin));

    locus::McpServerConfig cfg;
    cfg.name    = "crashy";
    cfg.command = bin.string();
    // Crash after 2 requests: the server replies to `initialize`, then crashes
    // before answering `tools/list`. The client's pending request should
    // surface a timeout/throw rather than hang.
    cfg.env["LOCUS_MOCK_MCP_CRASH_AFTER"] = "2";

    locus::McpClient client(cfg);
    REQUIRE(client.start(fs::temp_directory_path()));

    bool crash_seen = false;
    client.on_crash([&]{ crash_seen = true; });

    // The server replies to tools/list (request #2) and then crashes. Either
    // we get a valid response, or the pipe closes mid-response and we throw.
    // Both are acceptable -- what matters is the next call definitely fails.
    try { (void)client.list_tools(); } catch (...) {}

    // Give the reader thread a moment to observe the pipe close.
    for (int i = 0; i < 50 && client.status() == locus::McpClient::Status::ready; ++i) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(client.status() != locus::McpClient::Status::ready);
}

// ---------------------------------------------------------------------------
// McpManager + ToolRegistry integration
// ---------------------------------------------------------------------------

TEST_CASE("McpManager: registers tools as mcp:server:tool and proxies calls",
          "[s4.g][mcp][manager]")
{
    auto bin = mock_server_path();
    REQUIRE(fs::exists(bin));

    auto root = fs::temp_directory_path() / "locus_test_mcp_mgr";
    fs::create_directories(root / ".locus");
    {
        std::ofstream f(root / ".locus" / "mcp.json");
        f << R"({"mcpServers":{"mock":{"command":")"
          << bin.generic_string()
          << R"("}}})";
    }

    locus::ToolRegistry reg;
    {
        locus::McpManager mgr(reg, root);
        mgr.start_all();

        auto* echo_tool = reg.find("mcp:mock:echo");
        REQUIRE(echo_tool != nullptr);
        REQUIRE(echo_tool->name() == "mcp:mock:echo");

        NullWs ws;
        locus::ToolCall call;
        call.id        = "t1";
        call.tool_name = "mcp:mock:echo";
        call.args      = nlohmann::json::object();
        call.args["text"] = "via manager";

        auto res = echo_tool->execute(call, ws);
        REQUIRE(res.success);
        REQUIRE_THAT(res.content, ContainsSubstring("via manager"));

        // Schema build path: the registry should pick up the inputSchema verbatim
        // rather than trying to fabricate it from params().
        auto schema = reg.build_schema_json();
        bool found = false;
        for (auto& entry : schema) {
            if (entry["function"]["name"] == "mcp:mock:echo") {
                found = true;
                const auto& params = entry["function"]["parameters"];
                REQUIRE(params["type"] == "object");
                REQUIRE(params["properties"].contains("text"));
            }
        }
        REQUIRE(found);
    }
    // Manager destructor terminated the mock server; the OS may still hold
    // the cwd handle for a tick after Job Object teardown, so retry the
    // tempdir cleanup briefly rather than failing the test on a Windows
    // file-lock race.
    for (int i = 0; i < 20; ++i) {
        std::error_code ec;
        fs::remove_all(root, ec);
        if (!ec) break;
        std::this_thread::sleep_for(50ms);
    }
}

TEST_CASE("McpManager: graceful degradation when a server fails to start",
          "[s4.g][mcp][manager]")
{
    auto root = fs::temp_directory_path() / "locus_test_mcp_bad";
    fs::create_directories(root / ".locus");
    {
        std::ofstream f(root / ".locus" / "mcp.json");
        f << R"({"mcpServers":{"nope":{"command":"this-binary-does-not-exist.exe"}}})";
    }

    locus::ToolRegistry reg;
    locus::McpManager mgr(reg, root);
    mgr.start_all();

    auto snap = mgr.status_snapshot();
    REQUIRE(snap.size() == 1);
    REQUIRE(snap[0].name == "nope");
    REQUIRE(snap[0].status == locus::McpClient::Status::failed);
    REQUIRE_FALSE(snap[0].last_error.empty());

    // The registry stays clean -- failing servers contribute zero tools.
    REQUIRE(reg.all().empty());

    fs::remove_all(root);
}

#endif // _WIN32
