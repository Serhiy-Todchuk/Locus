#pragma once

#include "mcp/mcp_types.h"
#include "mcp/stdio_transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>

namespace locus {

// One MCP server instance. Owns the StdioTransport, performs the JSON-RPC
// handshake, and exposes synchronous list/call surfaces backed by a
// promise/future request map.
//
// Lifecycle:
//   ctor -> spawn(transport) -> initialize() -> ready
//   any time: pipe close -> on_closed -> requests fail with crashed status
//   dtor    -> terminate transport
class McpClient {
public:
    enum class Status { not_started, initializing, ready, failed, crashed, stopped };

    explicit McpClient(McpServerConfig cfg);
    ~McpClient();

    McpClient(const McpClient&)            = delete;
    McpClient& operator=(const McpClient&) = delete;

    // Spawn the child + run the `initialize` handshake. Blocks up to
    // cfg.init_timeout. Returns true on success; on failure status() ==
    // failed and last_error() carries the diagnostic.
    bool start(const std::filesystem::path& workspace_root);

    // Idempotent. Forces the transport closed; status() goes to stopped.
    void stop();

    // Synchronous JSON-RPC calls. Each blocks until the server responds
    // or the configured per-call timeout elapses. They throw
    // std::runtime_error on protocol or transport failure.
    std::vector<McpToolDefinition> list_tools(std::chrono::milliseconds timeout =
                                                  std::chrono::milliseconds(5000));

    // Returns the parsed `result` object from `tools/call`. Caller is
    // responsible for shaping it for the LLM (we hand back the raw MCP
    // result so the namespacing tool can decide on display vs content).
    nlohmann::json call_tool(const std::string& tool_name,
                             const nlohmann::json& arguments,
                             std::chrono::milliseconds timeout =
                                 std::chrono::milliseconds(60000));

    // Status accessors. Thread-safe.
    Status              status()       const;
    std::string         last_error()   const;
    const McpServerConfig& config()    const { return cfg_; }

    // Optional callback fired (on the transport reader thread) when the
    // server pipe closes after a previously-ready session. Used by
    // McpManager to unregister tools + emit a warning.
    void on_crash(std::function<void()> cb) { crash_cb_ = std::move(cb); }

private:
    void handle_message(std::string line);
    void handle_close();

    nlohmann::json send_request_sync(const std::string& method,
                                     nlohmann::json params,
                                     std::chrono::milliseconds timeout);

    McpServerConfig                  cfg_;
    std::unique_ptr<StdioTransport>  transport_;
    std::function<void()>            crash_cb_;

    std::atomic<Status>              status_{Status::not_started};
    mutable std::mutex               err_mu_;
    std::string                      last_error_;

    // Pending request map: id -> promise. The reader thread fulfils each
    // promise when a matching response arrives; send_request_sync waits on
    // its own future with a timeout.
    std::mutex                                                              pending_mu_;
    std::unordered_map<std::int64_t, std::shared_ptr<std::promise<nlohmann::json>>> pending_;
    std::atomic<std::int64_t>                                               next_id_{1};
};

const char* to_string(McpClient::Status s);

} // namespace locus
