#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace locus::jsonrpc {

// JSON-RPC 2.0 message kinds. The MCP stdio transport uses newline-delimited
// JSON: one message per line, each terminated by `\n`. (The spec also allows
// LSP-style Content-Length framing, but every shipping MCP stdio server
// today uses the newline form.)
enum class Kind { request, response, notification, parse_error, invalid };

struct Message {
    Kind                          kind = Kind::invalid;
    std::optional<std::int64_t>   id;          // present on request + response
    std::string                   method;       // present on request + notification
    nlohmann::json                params;       // request + notification (object/array/null)
    nlohmann::json                result;       // present on success response
    nlohmann::json                error;        // present on error response: {code, message, data?}
    std::string                   parse_error;  // human-readable, when kind == parse_error
};

// Build a JSON-RPC 2.0 request envelope. `id` is required by the spec.
inline nlohmann::json make_request(std::int64_t id,
                                   const std::string& method,
                                   nlohmann::json params = nlohmann::json::object())
{
    nlohmann::json msg;
    msg["jsonrpc"] = "2.0";
    msg["id"]      = id;
    msg["method"]  = method;
    msg["params"]  = std::move(params);
    return msg;
}

inline nlohmann::json make_notification(const std::string& method,
                                        nlohmann::json params = nlohmann::json::object())
{
    nlohmann::json msg;
    msg["jsonrpc"] = "2.0";
    msg["method"]  = method;
    msg["params"]  = std::move(params);
    return msg;
}

inline nlohmann::json make_response(std::int64_t id, nlohmann::json result)
{
    nlohmann::json msg;
    msg["jsonrpc"] = "2.0";
    msg["id"]      = id;
    msg["result"]  = std::move(result);
    return msg;
}

inline nlohmann::json make_error(std::int64_t id, int code, std::string message,
                                 nlohmann::json data = nullptr)
{
    nlohmann::json err;
    err["code"]    = code;
    err["message"] = std::move(message);
    if (!data.is_null()) err["data"] = std::move(data);

    nlohmann::json msg;
    msg["jsonrpc"] = "2.0";
    msg["id"]      = id;
    msg["error"]   = std::move(err);
    return msg;
}

// Parse a single JSON-RPC line. Tolerant of malformed input -- callers
// inspect Message::kind to decide whether to dispatch, log, or drop.
Message parse_message(const std::string& line);

} // namespace locus::jsonrpc
