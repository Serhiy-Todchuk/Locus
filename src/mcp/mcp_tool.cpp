#include "mcp/mcp_tool.h"

#include "core/workspace.h"
#include "core/workspace_services.h"
#include "security/injection_policy.h"
#include "security/injection_scanner.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>

namespace locus {

McpTool::McpTool(McpClient* client, McpToolDefinition def)
    : client_(client), def_(std::move(def)),
      server_name_(client_ ? client_->config().name : ""),
      namespaced_("mcp:" + server_name_ + ":" + def_.name)
{
}

std::string McpTool::description() const
{
    if (def_.description.empty())
        return "MCP tool '" + def_.name + "' from server '" + server_name_ + "'";
    return def_.description;
}

std::vector<ToolParam> McpTool::params() const
{
    // Best-effort flat projection of the JSON Schema for callers that bypass
    // parameters_schema() (e.g. tests, the system-prompt text builder). The
    // OpenAI tool-schema path uses parameters_schema() instead and gets the
    // full nested form.
    std::vector<ToolParam> out;
    if (!def_.input_schema.is_object()) return out;
    if (!def_.input_schema.contains("properties")) return out;

    const auto& props = def_.input_schema["properties"];
    if (!props.is_object()) return out;

    std::vector<std::string> required;
    if (def_.input_schema.contains("required") && def_.input_schema["required"].is_array()) {
        for (const auto& r : def_.input_schema["required"])
            if (r.is_string()) required.push_back(r.get<std::string>());
    }

    for (auto it = props.begin(); it != props.end(); ++it) {
        ToolParam p;
        p.name = it.key();
        p.type = it.value().value("type", std::string{"string"});
        p.description = it.value().value("description", std::string{});
        p.required = (std::find(required.begin(), required.end(), p.name) != required.end());
        out.push_back(std::move(p));
    }
    return out;
}

bool McpTool::available(IWorkspaceServices& /*ws*/) const
{
    return client_ && client_->status() == McpClient::Status::ready;
}

std::string McpTool::preview(const ToolCall& call) const
{
    std::ostringstream os;
    os << namespaced_;
    if (call.args.is_object() && !call.args.empty()) {
        // Truncate long argument dumps so the preview stays scannable.
        std::string dump = call.args.dump();
        if (dump.size() > 120) dump = dump.substr(0, 117) + "...";
        os << " " << dump;
    }
    return os.str();
}

namespace {

// Hard cap on the per-call result body we feed back into the chat. Matches
// the 1 MB-per-file cap CheckpointStore uses. A misbehaving / runaway MCP
// server returning 10 MB of text would otherwise bloat ConversationHistory
// before the context-budget compaction trigger has a chance to react.
constexpr std::size_t k_max_result_bytes = 1024 * 1024;

// Squash the MCP `tools/call` `result.content[]` array into a single string
// for the LLM-facing channel. MCP returns an array of typed parts (text,
// image, resource); we keep text parts verbatim and tag others by type.
std::string flatten_content(const nlohmann::json& result)
{
    if (!result.is_object()) return result.dump();

    if (!result.contains("content") || !result["content"].is_array()) {
        // Non-standard shape -- forward the whole result so the LLM at
        // least sees the response payload.
        return result.dump();
    }

    std::string out;
    for (const auto& part : result["content"]) {
        if (!part.is_object()) continue;
        std::string type = part.value("type", std::string{});
        if (type == "text") {
            out += part.value("text", std::string{});
            out += '\n';
        } else if (type == "resource") {
            // Resource contents are usually inlined as text/blob; surface
            // text directly, mark blobs by URI.
            const auto& res = part.value("resource", nlohmann::json::object());
            if (res.contains("text") && res["text"].is_string()) {
                out += res["text"].get<std::string>();
                out += '\n';
            } else {
                out += "[resource: " + res.value("uri", std::string{"<no uri>"}) + "]\n";
            }
        } else {
            out += "[" + type + " content omitted]\n";
        }
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

} // namespace

ToolResult McpTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                             const std::atomic<bool>* cancel_flag)
{
    if (!client_ || client_->status() != McpClient::Status::ready) {
        ToolResult r;
        r.success = false;
        r.content = "Error: MCP server '" + server_name_ + "' is not ready (" +
                    (client_ ? to_string(client_->status()) : "no client") + ")";
        r.display = r.content;
        return r;
    }

    nlohmann::json result;
    try {
        result = client_->call_tool(def_.name, call.args,
                                     std::chrono::milliseconds(60000),
                                     cancel_flag);
    } catch (const std::exception& e) {
        ToolResult r;
        r.success = false;
        r.content = std::string("Error calling MCP tool '") + namespaced_ + "': " + e.what();
        r.display = r.content;
        return r;
    }

    bool is_error = result.is_object() && result.value("isError", false);
    std::string body = flatten_content(result);

    // S6.0 -- MCP tool results are UNTRUSTED external ingress (the server is
    // third-party text the user did not review). Scan + wrap before the body
    // reaches the LLM. The scanner is a transparency tripwire, not a boundary
    // (the approval gate is); MCP tools default to `ask` so the user already
    // sees the call. On findings we fence the body with a per-call nonce + an
    // in-band untrusted note, and surface a banner via the activity log.
    std::string injection_banner;
    {
        security::SecurityConfig sec;
        if (auto* w = ws.workspace()) {
            const auto& s = w->config().security;
            sec.injection_scan   = s.injection_scan;
            sec.scan_zim         = s.scan_zim;
            sec.block_confidence = s.block_confidence;
            sec.max_scan_kb      = s.max_scan_kb;
        }
        if (sec.injection_scan && !body.empty()) {
            security::ScannerConfig scfg;
            scfg.max_scan_bytes = static_cast<std::size_t>(sec.max_scan_kb) * 1024;
            security::ScanResult scan = security::scan_for_injection(body, scfg);
            security::IngressDecision dec =
                security::apply_injection_policy(body, scan, sec);
            if (dec.action != security::IngressAction::Pass) {
                injection_banner = dec.banner;
                spdlog::warn("McpTool[{}]: injection scan -- {}",
                             namespaced_, dec.banner);
            }
        }
    }

    // Cap the per-call result body. Logged at warn level so the user sees
    // it in the activity panel; tag the suffix so the LLM understands the
    // tail is missing and avoids parroting partial content as authoritative.
    if (body.size() > k_max_result_bytes) {
        std::size_t dropped = body.size() - k_max_result_bytes;
        spdlog::warn("McpTool[{}]: result truncated from {} to {} bytes "
                     "({} dropped). Raise the cap or fix the server.",
                     namespaced_, body.size(), k_max_result_bytes, dropped);
        body.resize(k_max_result_bytes);
        body += "\n\n[truncated " + std::to_string(dropped) +
                " bytes -- MCP result exceeded the 1 MB cap]";
    }

    ToolResult r;
    r.success = !is_error;
    r.content = body;
    r.display = body;
    if (is_error) {
        r.content = "Error from MCP server: " + body;
        r.display = r.content;
    }
    if (!injection_banner.empty()) {
        // Emit a distinct activity event so the Details panel logs the scan.
        r.activity_tag     = "injection_scan";
        r.activity_summary = injection_banner;
        r.activity_detail  = "Untrusted MCP result from '" + server_name_ +
                             "' was wrapped. " + injection_banner;
    }
    return r;
}

} // namespace locus
