#include "mcp/mcp_client.h"

#include "mcp/json_rpc.h"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <utility>

namespace locus {

namespace fs = std::filesystem;

const char* to_string(McpClient::Status s)
{
    switch (s) {
        case McpClient::Status::not_started:  return "not_started";
        case McpClient::Status::initializing: return "initializing";
        case McpClient::Status::ready:        return "ready";
        case McpClient::Status::failed:       return "failed";
        case McpClient::Status::crashed:      return "crashed";
        case McpClient::Status::stopped:      return "stopped";
    }
    return "unknown";
}

McpClient::McpClient(McpServerConfig cfg)
    : cfg_(std::move(cfg))
{
}

McpClient::~McpClient()
{
    stop();
}

void McpClient::stop()
{
    Status prev = status_.exchange(Status::stopped);
    if (prev == Status::stopped) return;

    if (transport_) transport_->terminate();

    // Fail any outstanding requests so callers don't deadlock waiting.
    decltype(pending_) drained;
    {
        std::lock_guard l(pending_mu_);
        drained.swap(pending_);
    }
    for (auto& [id, prom] : drained) {
        try {
            prom->set_exception(std::make_exception_ptr(
                std::runtime_error("MCP client stopped before response arrived")));
        } catch (...) {}
    }
}

bool McpClient::start(const fs::path& workspace_root)
{
    status_ = Status::initializing;

    // Fresh start cycle: reset cached exit info from any previous run.
    exit_code_.store(0);
    has_exit_code_.store(false);

    transport_ = std::make_unique<StdioTransport>();
    transport_->on_message([this](std::string line) { handle_message(std::move(line)); });
    transport_->on_closed ([this](int code, bool have) { handle_close(code, have); });

    fs::path cwd = cfg_.cwd.empty() ? workspace_root : fs::path(cfg_.cwd);
    try {
        transport_->spawn(cfg_.command, cfg_.args, cfg_.env, cwd);
    } catch (const std::exception& e) {
        std::lock_guard l(err_mu_);
        last_error_ = std::string("spawn failed: ") + e.what();
        status_ = Status::failed;
        spdlog::warn("McpClient[{}]: {}", cfg_.name, last_error_);
        return false;
    }

    // Send `initialize`. Per spec the params are:
    //   { protocolVersion, capabilities, clientInfo: { name, version } }
    nlohmann::json params;
    params["protocolVersion"] = "2025-06-18";
    params["capabilities"]    = nlohmann::json::object();
    params["clientInfo"]      = { {"name", "Locus"}, {"version", "0.1.0"} };

    try {
        auto result = send_request_sync("initialize", params, cfg_.init_timeout);
        std::string server_name = "<unknown>";
        if (result.is_object() && result.contains("serverInfo") &&
            result["serverInfo"].is_object() &&
            result["serverInfo"].value("name", "").size() > 0) {
            server_name = result["serverInfo"]["name"].get<std::string>();
        }
        spdlog::info("McpClient[{}]: initialised, server: {}", cfg_.name, server_name);
    } catch (const std::exception& e) {
        std::lock_guard l(err_mu_);
        last_error_ = std::string("initialize failed: ") + e.what();
        status_ = Status::failed;
        spdlog::warn("McpClient[{}]: {}", cfg_.name, last_error_);
        if (transport_) transport_->terminate();
        return false;
    }

    // Spec requires a `notifications/initialized` notification after the
    // `initialize` response. Best-effort -- we don't fail start() if it
    // can't go through, since some servers tolerate its absence.
    nlohmann::json initialized = jsonrpc::make_notification("notifications/initialized");
    transport_->send_line(initialized.dump());

    status_ = Status::ready;
    return true;
}

std::vector<McpToolDefinition>
McpClient::list_tools(std::chrono::milliseconds timeout)
{
    if (status_.load() != Status::ready)
        throw std::runtime_error("McpClient not ready");

    auto result = send_request_sync("tools/list", nlohmann::json::object(), timeout);

    std::vector<McpToolDefinition> out;
    if (!result.contains("tools") || !result["tools"].is_array()) return out;

    for (const auto& t : result["tools"]) {
        if (!t.is_object() || !t.contains("name") || !t["name"].is_string()) continue;
        McpToolDefinition def;
        def.name        = t["name"].get<std::string>();
        def.description = t.value("description", std::string{});
        if (t.contains("inputSchema") && t["inputSchema"].is_object())
            def.input_schema = t["inputSchema"];
        else
            def.input_schema = { {"type", "object"}, {"properties", nlohmann::json::object()} };
        out.push_back(std::move(def));
    }
    return out;
}

nlohmann::json McpClient::call_tool(const std::string& tool_name,
                                    const nlohmann::json& arguments,
                                    std::chrono::milliseconds timeout)
{
    if (status_.load() != Status::ready)
        throw std::runtime_error("McpClient not ready");

    nlohmann::json params;
    params["name"]      = tool_name;
    params["arguments"] = arguments.is_null() ? nlohmann::json::object() : arguments;

    return send_request_sync("tools/call", std::move(params), timeout);
}

McpClient::Status McpClient::status() const { return status_.load(); }

std::string McpClient::last_error() const
{
    std::lock_guard l(err_mu_);
    return last_error_;
}

nlohmann::json McpClient::send_request_sync(const std::string& method,
                                            nlohmann::json params,
                                            std::chrono::milliseconds timeout)
{
    std::int64_t id = next_id_.fetch_add(1);
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future  = promise->get_future();

    {
        std::lock_guard l(pending_mu_);
        pending_[id] = promise;
    }

    nlohmann::json msg = jsonrpc::make_request(id, method, std::move(params));
    if (!transport_ || !transport_->send_line(msg.dump())) {
        std::lock_guard l(pending_mu_);
        pending_.erase(id);
        throw std::runtime_error("transport write failed");
    }

    if (future.wait_for(timeout) != std::future_status::ready) {
        std::lock_guard l(pending_mu_);
        pending_.erase(id);
        throw std::runtime_error("timed out waiting for response to " + method);
    }
    return future.get();
}

void McpClient::handle_message(std::string line)
{
    auto msg = jsonrpc::parse_message(line);
    if (msg.kind == jsonrpc::Kind::parse_error || msg.kind == jsonrpc::Kind::invalid) {
        spdlog::warn("McpClient[{}]: dropping malformed message: {}", cfg_.name,
                     msg.parse_error.empty() ? "?" : msg.parse_error);
        return;
    }

    if (msg.kind == jsonrpc::Kind::response && msg.id.has_value()) {
        std::shared_ptr<std::promise<nlohmann::json>> prom;
        {
            std::lock_guard l(pending_mu_);
            auto it = pending_.find(*msg.id);
            if (it == pending_.end()) {
                spdlog::trace("McpClient[{}]: response for unknown id {}", cfg_.name, *msg.id);
                return;
            }
            prom = it->second;
            pending_.erase(it);
        }
        try {
            if (!msg.error.is_null()) {
                std::string em = msg.error.value("message", std::string{"unknown"});
                int code = msg.error.value("code", 0);
                prom->set_exception(std::make_exception_ptr(
                    std::runtime_error("MCP error " + std::to_string(code) + ": " + em)));
            } else {
                prom->set_value(msg.result);
            }
        } catch (...) {}
        return;
    }

    // Notifications + server-initiated requests: log at trace level. We
    // don't currently handle progress / log notifications, but they're
    // valid messages to receive.
    if (msg.kind == jsonrpc::Kind::notification) {
        spdlog::trace("McpClient[{}]: notification {}", cfg_.name, msg.method);
        return;
    }
    if (msg.kind == jsonrpc::Kind::request) {
        spdlog::trace("McpClient[{}]: server->client request {} (ignored)",
                      cfg_.name, msg.method);
    }
}

void McpClient::handle_close(int exit_code, bool had_exit_code)
{
    // Cache the exit info before we touch status_, so a UI listener that
    // reads back exit_code() / has_exit_code() in its status callback sees
    // the real values rather than stale defaults.
    if (had_exit_code) {
        exit_code_.store(exit_code);
        has_exit_code_.store(true);
    }

    Status prev = status_.load();
    if (prev == Status::stopped) return;     // we initiated the shutdown
    if (prev == Status::not_started) return;

    {
        std::lock_guard l(err_mu_);
        if (last_error_.empty()) {
            if (had_exit_code)
                last_error_ = "child process exited (code " +
                              std::to_string(exit_code) + ")";
            else
                last_error_ = "child process exited";
        }
    }

    Status new_status = (prev == Status::initializing) ? Status::failed : Status::crashed;
    status_ = new_status;

    // Fail every outstanding request so synchronous callers unblock.
    decltype(pending_) drained;
    {
        std::lock_guard l(pending_mu_);
        drained.swap(pending_);
    }
    for (auto& [id, prom] : drained) {
        try {
            prom->set_exception(std::make_exception_ptr(
                std::runtime_error("MCP server exited before response")));
        } catch (...) {}
    }

    spdlog::warn("McpClient[{}]: server pipe closed -> {}", cfg_.name, to_string(new_status));

    if (new_status == Status::crashed && crash_cb_) {
        try { crash_cb_(); } catch (...) {}
    }
}

} // namespace locus
