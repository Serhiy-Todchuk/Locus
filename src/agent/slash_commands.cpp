#include "slash_commands.h"

#include "prompt_templates.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>

namespace locus {

namespace {

std::string trim_left(std::string s)
{
    auto i = s.find_first_not_of(" \t");
    if (i == std::string::npos) return {};
    return s.substr(i);
}

bool ieq_prefix(std::string_view hay, std::string_view needle)
{
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i < needle.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(hay[i])) !=
            std::tolower(static_cast<unsigned char>(needle[i])))
            return false;
    }
    return true;
}

bool icontains(std::string_view hay, std::string_view needle)
{
    if (needle.empty()) return true;
    auto n = needle.size();
    if (hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

// Shell-style tokenizer: whitespace separates tokens, double quotes preserve
// spaces. Mid-token quotes are spliced in (`a="b c"d` → `ab cd`). Throws
// SlashParseError on unterminated quote.
std::vector<std::string> tokenize(std::string_view s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;

        std::string tok;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            if (s[i] == '"') {
                ++i;
                bool closed = false;
                while (i < s.size()) {
                    if (s[i] == '"') { ++i; closed = true; break; }
                    tok += s[i++];
                }
                if (!closed)
                    throw SlashParseError("unterminated quoted string");
            } else {
                tok += s[i++];
            }
        }
        out.push_back(std::move(tok));
    }
    return out;
}

// Checked integer parse. Throws SlashParseError on overflow or garbage.
int parse_int(const std::string& s, const std::string& field)
{
    if (s.empty())
        throw SlashParseError("argument '" + field + "': expected integer, got empty string");
    int value = 0;
    auto* first = s.data();
    auto* last  = s.data() + s.size();
    auto res = std::from_chars(first, last, value);
    if (res.ec != std::errc() || res.ptr != last)
        throw SlashParseError("argument '" + field + "': expected integer, got '" + s + "'");
    return value;
}

bool parse_bool(const std::string& s, const std::string& field)
{
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
        return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
        return false;
    throw SlashParseError("argument '" + field + "': expected boolean, got '" + s + "'");
}

void assign_typed(nlohmann::json& args,
                  const std::string& key,
                  const std::string& value,
                  const std::vector<ToolParam>& params)
{
    for (const auto& p : params) {
        if (p.name != key) continue;
        if (p.type == "integer")      args[key] = parse_int(value, key);
        else if (p.type == "boolean") args[key] = parse_bool(value, key);
        else                          args[key] = value;
        return;
    }
    // Unknown param name — pass through as string; the tool validates.
    args[key] = value;
}

std::string build_signature(const std::vector<ToolParam>& params)
{
    std::string sig;
    bool first = true;
    for (const auto& p : params) {
        if (!first) sig += " ";
        first = false;
        if (p.required) sig += "<" + p.name + ">";
        else            sig += "[" + p.name + "=" + p.type + "]";
    }
    return sig;
}

} // namespace

// -- Parser -----------------------------------------------------------------

std::optional<ParsedSlashCall> SlashCommandParser::parse(std::string_view input,
                                                         const IToolRegistry& tools)
{
    if (input.empty() || input.front() != '/')
        return std::nullopt;

    // Strip leading slash, then split into "name<space>rest".
    std::string body(input.substr(1));
    size_t ws = body.find_first_of(" \t");
    std::string name = (ws == std::string::npos) ? body : body.substr(0, ws);
    std::string rest = (ws == std::string::npos) ? std::string{} : trim_left(body.substr(ws));

    if (name.empty())
        return std::nullopt;  // just "/" or "/   "

    if (name == "help") {
        ParsedSlashCall call;
        call.is_help = true;
        return call;
    }

    ParsedSlashCall call;
    call.tool_name = name;

    const ITool* tool = tools.find(name);
    const std::vector<ToolParam> params = tool ? tool->params() : std::vector<ToolParam>{};

    auto tokens = tokenize(rest);

    std::vector<std::string> positional;
    std::unordered_map<std::string, std::string> kwargs;
    for (auto& tok : tokens) {
        auto eq = tok.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = tok.substr(0, eq);
            std::string val = tok.substr(eq + 1);
            assign_typed(call.args, key, val, params);
            kwargs[key] = std::move(val);
        } else {
            positional.push_back(std::move(tok));
        }
    }

    size_t pos_idx = 0;
    for (const auto& p : params) {
        if (call.args.contains(p.name)) continue;
        if (pos_idx >= positional.size()) break;
        assign_typed(call.args, p.name, positional[pos_idx], params);
        ++pos_idx;
    }

    call.positional = std::move(positional);
    call.kwargs     = std::move(kwargs);
    return call;
}

// -- Dispatcher -------------------------------------------------------------

SlashCommandDispatcher::SlashCommandDispatcher(IToolRegistry& tools,
                                               IWorkspaceServices& services)
    : tools_(tools)
    , services_(services)
{}

bool SlashCommandDispatcher::try_dispatch(
    std::string_view input,
    const std::function<void(std::string)>& on_output,
    const std::function<void(std::string)>& on_error)
{
    std::optional<ParsedSlashCall> parsed;
    try {
        parsed = SlashCommandParser::parse(input, tools_);
    } catch (const SlashParseError& ex) {
        on_error(std::string("Slash command error: ") + ex.what());
        return true;
    }

    if (!parsed) return false;

    if (parsed->is_help) {
        on_output(render_help());
        return true;
    }

    ITool* tool = tools_.find(parsed->tool_name);
    if (!tool) {
        on_error("Unknown command '/" + parsed->tool_name +
                 "'. Type /help for available commands.");
        return true;
    }

    ToolCall call;
    call.id        = "slash_" + parsed->tool_name;
    call.tool_name = parsed->tool_name;
    call.args      = parsed->args;

    spdlog::info("Slash command: /{} args={}", parsed->tool_name, call.args.dump());

    auto t0 = std::chrono::steady_clock::now();
    ToolResult result = tool->execute(call, services_);
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    on_output(format_result(parsed->tool_name, result, ms));
    return true;
}

std::vector<SlashCompletion> SlashCommandDispatcher::complete(std::string_view prefix) const
{
    std::vector<SlashCompletion> prefix_hits, substr_hits;
    for (auto* t : tools_.all()) {
        SlashCompletion c;
        c.name        = t->name();
        c.description = t->description();
        c.signature   = build_signature(t->params());
        c.kind        = "tool";

        if (prefix.empty()) {
            prefix_hits.push_back(std::move(c));
        } else if (ieq_prefix(c.name, prefix)) {
            prefix_hits.push_back(std::move(c));
        } else if (icontains(c.name, prefix)) {
            substr_hits.push_back(std::move(c));
        }
    }

    if (templates_) {
        for (auto& tpl : templates_->list()) {
            // Skip names already covered by a real tool: built-ins win, per
            // the S4.X spec ("a malicious template named `compact` can't
            // shadow `/compact`").
            if (tools_.find(tpl.name)) continue;

            SlashCompletion c;
            c.name        = tpl.name;
            c.description = tpl.description.empty()
                ? std::string("(prompt template)")
                : tpl.description;
            // Synthesise a signature from declared args, if any.
            if (!tpl.args.empty()) {
                std::string sig;
                for (size_t i = 0; i < tpl.args.size(); ++i) {
                    if (i) sig += " ";
                    sig += "<" + tpl.args[i] + ">";
                }
                c.signature = std::move(sig);
            }
            c.kind = "template";

            if (prefix.empty()) {
                prefix_hits.push_back(std::move(c));
            } else if (ieq_prefix(c.name, prefix)) {
                prefix_hits.push_back(std::move(c));
            } else if (icontains(c.name, prefix)) {
                substr_hits.push_back(std::move(c));
            }
        }
    }

    prefix_hits.insert(prefix_hits.end(),
                       std::make_move_iterator(substr_hits.begin()),
                       std::make_move_iterator(substr_hits.end()));
    return prefix_hits;
}

std::string SlashCommandDispatcher::render_help() const
{
    std::string help = "Available /commands (direct tool invocation):\n\n";
    for (auto* t : tools_.all()) {
        help += "  /" + t->name();
        auto sig = build_signature(t->params());
        if (!sig.empty()) help += " " + sig;
        help += "\n    " + t->description() + "\n\n";
    }
    help += "  /help\n    Show this help\n";
    help += "  /reload\n    Re-scan prompt templates from disk\n";

    // S4.X -- prompt templates grouped by scope, built-ins first (above).
    if (templates_) {
        auto entries = templates_->list();
        if (!entries.empty()) {
            // Split into project / global, drop entries shadowed by a tool.
            std::vector<const PromptTemplate*> project, global;
            for (auto& e : entries) {
                if (tools_.find(e.name)) continue;
                (e.is_project ? project : global).push_back(&e);
            }
            auto render_group = [&](const char* heading,
                                    const std::vector<const PromptTemplate*>& group) {
                if (group.empty()) return;
                help += "\n";
                help += heading;
                help += "\n\n";
                for (auto* tpl : group) {
                    help += "  /" + tpl->name;
                    if (!tpl->args.empty()) {
                        for (auto& a : tpl->args) help += " <" + a + ">";
                    }
                    help += "\n";
                    if (!tpl->description.empty())
                        help += "    " + tpl->description + "\n";
                }
            };
            render_group("Templates (project):", project);
            render_group("Templates (global):",  global);
        }
    }
    return help;
}

std::string SlashCommandDispatcher::format_result(const std::string& tool_name,
                                                  const ToolResult& result,
                                                  long long ms) const
{
    std::string out = "**/" + tool_name + "** ";
    out += result.success ? "(OK, " : "(FAILED, ";
    out += std::to_string(ms) + "ms)\n\n";
    out += result.display.empty() ? result.content : result.display;
    return out;
}

} // namespace locus
