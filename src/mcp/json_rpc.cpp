#include "mcp/json_rpc.h"

namespace locus::jsonrpc {

Message parse_message(const std::string& line)
{
    Message m;
    if (line.empty()) {
        m.kind = Kind::invalid;
        m.parse_error = "empty line";
        return m;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception& e) {
        m.kind = Kind::parse_error;
        m.parse_error = e.what();
        return m;
    }
    if (!j.is_object()) {
        m.kind = Kind::invalid;
        m.parse_error = "not a JSON object";
        return m;
    }

    if (j.contains("id") && !j["id"].is_null()) {
        // Per spec id may be string, number, or null. We coerce to int64
        // since we only originate numeric ids ourselves; non-numeric ids
        // from a server response are unexpected.
        if (j["id"].is_number_integer())
            m.id = j["id"].get<std::int64_t>();
        else if (j["id"].is_number_unsigned())
            m.id = static_cast<std::int64_t>(j["id"].get<std::uint64_t>());
        // String ids get stringified for error reporting only; we don't
        // route by them.
    }

    const bool has_method = j.contains("method") && j["method"].is_string();
    const bool has_result = j.contains("result");
    const bool has_error  = j.contains("error");

    if (has_method) {
        m.method = j["method"].get<std::string>();
        if (j.contains("params"))
            m.params = j["params"];
        m.kind = m.id.has_value() ? Kind::request : Kind::notification;
        return m;
    }

    if (has_result || has_error) {
        if (has_result) m.result = j["result"];
        if (has_error)  m.error  = j["error"];
        m.kind = Kind::response;
        return m;
    }

    m.kind = Kind::invalid;
    m.parse_error = "no method, result, or error";
    return m;
}

} // namespace locus::jsonrpc
