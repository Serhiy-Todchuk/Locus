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
#include "tools/shared.h"
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

TEST_CASE("mcp_config: tolerates // and /* */ comments (jsonc)",
          "[s4.g][mcp][config]")
{
    // Real-world: users paste configs across Claude Desktop / Cursor / Cline /
    // VS Code, all of which accept comments. Locus must not reject the file
    // just because a `//` comment slipped in.
    std::string body = R"({
        "mcpServers": {
            // a server registered for kicks
            "fs": { "command": "node", "args": ["server.js"] }
            /* multi-line
               comment block */
        }
    })";
    auto cfgs = locus::McpConfigLoader::parse_json(body);
    REQUIRE(cfgs.size() == 1);
    REQUIRE(cfgs[0].name == "fs");
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

TEST_CASE("McpManager: stop_all unregisters MCP tools from the registry",
          "[s4.g][mcp][manager]")
{
    auto bin = mock_server_path();
    REQUIRE(fs::exists(bin));

    auto root = fs::temp_directory_path() / "locus_test_mcp_stop";
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
        REQUIRE(reg.find("mcp:mock:echo") != nullptr);

        mgr.stop_all();

        // Phase 2 contract: stop_all unregisters every tool the manager
        // had registered so the agent's tool registry can't hand out
        // McpTool entries that borrow a torn-down client pointer.
        REQUIRE(reg.find("mcp:mock:echo") == nullptr);
        REQUIRE(reg.find("mcp:mock:fail_once") == nullptr);
    }
    for (int i = 0; i < 20; ++i) {
        std::error_code ec;
        fs::remove_all(root, ec);
        if (!ec) break;
        std::this_thread::sleep_for(50ms);
    }
}

TEST_CASE("resolve_approval_policy: exact match, prefix match, fall-through",
          "[s4.g][tools][approval]")
{
    using locus::ToolApprovalPolicy;
    using locus::tools::resolve_approval_policy;
    using Map = std::unordered_map<std::string, ToolApprovalPolicy>;

    // No overrides -> tool default returned.
    REQUIRE(resolve_approval_policy("read_file", ToolApprovalPolicy::auto_approve, {})
            == ToolApprovalPolicy::auto_approve);

    // Exact match beats default.
    Map exact = { {"read_file", ToolApprovalPolicy::deny} };
    REQUIRE(resolve_approval_policy("read_file", ToolApprovalPolicy::auto_approve, exact)
            == ToolApprovalPolicy::deny);

    // Prefix "mcp:fs:*" matches "mcp:fs:read_file".
    Map prefix = { {"mcp:fs:*", ToolApprovalPolicy::auto_approve} };
    REQUIRE(resolve_approval_policy("mcp:fs:read_file", ToolApprovalPolicy::ask, prefix)
            == ToolApprovalPolicy::auto_approve);

    // But does NOT bleed into a different server name.
    REQUIRE(resolve_approval_policy("mcp:other:read_file", ToolApprovalPolicy::ask, prefix)
            == ToolApprovalPolicy::ask);

    // Exact override beats prefix override.
    Map both = {
        {"mcp:fs:*",            ToolApprovalPolicy::auto_approve},
        {"mcp:fs:delete_file",  ToolApprovalPolicy::deny}
    };
    REQUIRE(resolve_approval_policy("mcp:fs:read_file",   ToolApprovalPolicy::ask, both)
            == ToolApprovalPolicy::auto_approve);
    REQUIRE(resolve_approval_policy("mcp:fs:delete_file", ToolApprovalPolicy::ask, both)
            == ToolApprovalPolicy::deny);

    // Bare "*" is intentionally NOT supported. It falls through.
    Map bare_star = { {"*", ToolApprovalPolicy::auto_approve} };
    REQUIRE(resolve_approval_policy("read_file", ToolApprovalPolicy::ask, bare_star)
            == ToolApprovalPolicy::ask);
}

TEST_CASE("ToolRegistry: unregister removes by name", "[s4.g][tools]")
{
    struct DummyTool : public locus::ITool {
        std::string n_;
        explicit DummyTool(std::string n) : n_(std::move(n)) {}
        std::string name()        const override { return n_; }
        std::string description() const override { return "dummy"; }
        std::vector<locus::ToolParam> params() const override { return {}; }
        locus::ToolResult execute(const locus::ToolCall&, locus::IWorkspaceServices&) override
        { return {true, "ok", "ok"}; }
    };

    locus::ToolRegistry reg;
    reg.register_tool(std::make_unique<DummyTool>("a"));
    reg.register_tool(std::make_unique<DummyTool>("b"));
    REQUIRE(reg.all().size() == 2);

    REQUIRE(reg.unregister_tool("a"));
    REQUIRE(reg.find("a") == nullptr);
    REQUIRE(reg.find("b") != nullptr);
    REQUIRE(reg.all().size() == 1);

    // Idempotent: second call returns false.
    REQUIRE_FALSE(reg.unregister_tool("a"));

    // Re-registering the same name is now allowed.
    reg.register_tool(std::make_unique<DummyTool>("a"));
    REQUIRE(reg.find("a") != nullptr);
    REQUIRE(reg.all().size() == 2);
}

TEST_CASE("McpTool: caps oversized result body at 1 MB", "[s4.g][mcp][tool]")
{
    // Build a fake McpClient-style result manually so we don't have to
    // teach the mock server to emit huge payloads. We exercise the cap
    // by invoking flatten_content + truncation through McpTool::execute --
    // but execute() takes a real client. Easier: drive the truncation
    // path by constructing a content string and asserting the size cap is
    // declared in the source by re-reading it from the binary's perspective.
    //
    // Pragmatic approach: invoke McpTool against a real mock-server call
    // where the result is small, and separately assert the cap constant is
    // applied by checking the visible truncation marker for a large-input
    // round-trip. The mock server echoes text verbatim, so we send 2 MB and
    // expect ~1 MB back with the truncation suffix.
    auto bin = mock_server_path();
    REQUIRE(fs::exists(bin));

    locus::McpServerConfig cfg;
    cfg.name    = "mock";
    cfg.command = bin.string();

    locus::McpClient client(cfg);
    REQUIRE(client.start(fs::temp_directory_path()));

    locus::McpToolDefinition def;
    def.name = "echo";
    def.input_schema = { {"type", "object"} };
    locus::McpTool tool(&client, def);

    locus::ToolCall call;
    call.id        = "t-cap";
    call.tool_name = "mcp:mock:echo";
    call.args      = nlohmann::json::object();
    call.args["text"] = std::string(2 * 1024 * 1024, 'x');  // 2 MB

    NullWs ws;
    auto r = tool.execute(call, ws);
    REQUIRE(r.success);
    REQUIRE(r.content.size() < 2 * 1024 * 1024);
    REQUIRE_THAT(r.content, ContainsSubstring("[truncated"));
    REQUIRE_THAT(r.content, ContainsSubstring("1 MB cap"));
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
