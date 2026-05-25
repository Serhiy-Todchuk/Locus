#include "llm/xml_tool_call_extractor.h"

#include "llm/json_repair.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <utility>

using json = nlohmann::json;

namespace locus {

namespace {

constexpr const char* k_qwen_open    = "<tool_call>";
constexpr const char* k_qwen_close   = "</tool_call>";
constexpr const char* k_claude_open  = "<function_calls>";
constexpr const char* k_claude_close = "</function_calls>";

// Largest k such that suffix of `s` of length k matches prefix of any of
// the listed marker strings (case-sensitive). Lets us hold back the
// rightmost few bytes of buf_ when they could complete an opening tag in
// the next chunk. Returns 0 when no suffix overlap is possible.
size_t partial_marker_suffix(const std::string& s,
                             const std::vector<const char*>& markers)
{
    size_t best = 0;
    for (auto* m : markers) {
        size_t mlen = std::strlen(m);
        size_t maxk = std::min<size_t>(s.size(), mlen - 1);
        for (size_t k = maxk; k > best; --k) {
            if (std::memcmp(s.data() + s.size() - k, m, k) == 0) {
                best = k;
                break;
            }
        }
    }
    return best;
}

// Trim ASCII whitespace from both ends.
std::string trim(std::string s)
{
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back()))  s.pop_back();
    return s;
}

// Synthesize a deterministic id for an XML-extracted tool call so the
// tool-result message can refer back to it. The format mirrors LM Studio's
// "call_xxx" style; the suffix is just an integer.
std::string synth_id(int idx)
{
    return "call_xml_" + std::to_string(idx);
}

// Decode a small set of XML entities used inside Claude <parameter> bodies.
// Models rarely emit these, but anyone forwarding plain-text values that
// happen to contain "&" / "<" / ">" will encode them, and we should restore.
std::string decode_entities(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '&') {
            // longest match wins
            auto starts_with = [&](const char* lit) {
                size_t n = std::strlen(lit);
                return i + n <= s.size() && std::memcmp(s.data() + i, lit, n) == 0;
            };
            if (starts_with("&amp;"))  { out += '&'; i += 5; continue; }
            if (starts_with("&lt;"))   { out += '<'; i += 4; continue; }
            if (starts_with("&gt;"))   { out += '>'; i += 4; continue; }
            if (starts_with("&quot;")) { out += '"'; i += 6; continue; }
            if (starts_with("&apos;")) { out += '\''; i += 6; continue; }
        }
        out += s[i++];
    }
    return out;
}

} // namespace

// ============================================================================

XmlToolCallExtractor::XmlToolCallExtractor(std::vector<XmlMarker> watch)
    : watch_(std::move(watch))
{
}

void XmlToolCallExtractor::reset()
{
    state_ = State::Outside;
    buf_.clear();
    next_index_ = 0;
}

void XmlToolCallExtractor::feed(const std::string& chunk,
                                const OnText& on_text,
                                const OnToolCall& on_tool_call)
{
    buf_ += chunk;

    // Loop: a single chunk may close one tool call and open another, or
    // contain multiple complete <tool_call> blocks.
    for (;;) {
        if (state_ == State::Outside) {
            if (!try_open(on_text)) {
                emit_safe_outside_text(on_text);
                break;
            }
            // Fell through to inside state; loop to look for closing tag.
        } else {
            if (!try_close(on_tool_call)) {
                // No close yet -- keep buffering until next chunk.
                break;
            }
            // After closing we're back outside; loop again to flush any
            // trailing text or detect another opener in the same chunk.
        }
    }
}

void XmlToolCallExtractor::finish(const OnText& on_text)
{
    if (state_ != State::Outside) {
        // Stream truncated mid-tool-call. Best-effort: forward what we
        // buffered as plain text so the user sees something. Don't try
        // to parse partial JSON.
        if (!buf_.empty() && on_text)
            on_text(buf_);
    } else {
        // Outside: flush whatever was being held back as a partial-tag
        // suffix, since no more bytes are coming.
        if (!buf_.empty() && on_text)
            on_text(buf_);
    }
    buf_.clear();
    state_ = State::Outside;
}

bool XmlToolCallExtractor::try_open(const OnText& on_text)
{
    // Find the earliest opening token among the watched dialects.
    size_t pos      = std::string::npos;
    State  next_st  = State::Outside;

    auto consider = [&](XmlMarker m, const char* tag, State target) {
        bool watched =
            std::find(watch_.begin(), watch_.end(), m) != watch_.end();
        if (!watched) return;
        size_t p = buf_.find(tag);
        if (p != std::string::npos && (pos == std::string::npos || p < pos)) {
            pos = p;
            next_st = target;
        }
    };
    consider(XmlMarker::Qwen,   k_qwen_open,   State::InQwen);
    consider(XmlMarker::Claude, k_claude_open, State::InClaude);

    if (pos == std::string::npos)
        return false;

    // Emit the prefix before the marker as text; drop the marker.
    if (pos > 0 && on_text)
        on_text(buf_.substr(0, pos));

    size_t skip = (next_st == State::InQwen)
        ? std::strlen(k_qwen_open)
        : std::strlen(k_claude_open);
    buf_.erase(0, pos + skip);
    state_ = next_st;
    return true;
}

bool XmlToolCallExtractor::try_close(const OnToolCall& on_tool_call)
{
    const char* close_tag = (state_ == State::InQwen)
        ? k_qwen_close : k_claude_close;
    size_t cp = buf_.find(close_tag);
    if (cp == std::string::npos)
        return false;

    std::string body = buf_.substr(0, cp);
    buf_.erase(0, cp + std::strlen(close_tag));

    if (state_ == State::InQwen)
        parse_qwen_body(body, next_index_, on_tool_call);
    else
        parse_claude_body(body, next_index_, on_tool_call);

    state_ = State::Outside;
    return true;
}

void XmlToolCallExtractor::emit_safe_outside_text(const OnText& on_text)
{
    if (buf_.empty() || !on_text) return;

    // Hold back any suffix that might be the start of a watched marker
    // that completes in the next chunk.
    std::vector<const char*> markers;
    for (auto m : watch_) {
        if (m == XmlMarker::Qwen)   markers.push_back(k_qwen_open);
        if (m == XmlMarker::Claude) markers.push_back(k_claude_open);
    }
    size_t hold = partial_marker_suffix(buf_, markers);
    if (hold >= buf_.size()) return;   // entire buffer might be a marker

    on_text(buf_.substr(0, buf_.size() - hold));
    buf_.erase(0, buf_.size() - hold);
}

// ----- Qwen body parsing -----------------------------------------------------

void XmlToolCallExtractor::parse_qwen_body(const std::string& body,
                                           int& next_index,
                                           const OnToolCall& on_tool_call)
{
    std::string trimmed = trim(body);
    if (trimmed.empty()) {
        spdlog::warn("XmlToolCallExtractor: empty <tool_call> body");
        return;
    }

    // Hermes / GLM dialect: some tunes wrap a non-JSON body inside
    // <tool_call>, using <function=NAME><parameter=...>... instead of the
    // canonical Qwen JSON object. Dispatch before json::parse so we don't
    // spuriously warn on a legitimately-formatted (just wrong-dialect) call.
    if (trimmed.compare(0, 9, "<function") == 0) {
        parse_hermes_body(trimmed, next_index, on_tool_call);
        return;
    }

    // Qwen body shape:
    //   {"name": "fn_name", "arguments": {"k": "v"}}
    // OR
    //   {"name": "fn_name", "arguments": "{\"k\":\"v\"}"}
    // Some streamers emit multiple JSON objects back-to-back inside one
    // <tool_call>; we don't handle that -- spec is one call per block.
    json j;
    try {
        j = json::parse(trimmed);
    } catch (const json::exception& e) {
        // S6.10 Task A -- JSON repair pre-pass. Small models routinely emit
        // trailing commas, unquoted keys, or close-brace-off bodies inside
        // <tool_call>. Try the repair pipeline before giving up.
        if (auto repaired = repair_for_parse(trimmed)) {
            spdlog::trace("XmlToolCallExtractor: Qwen JSON repaired via [{}]",
                          repaired->stages_applied);
            try {
                j = json::parse(repaired->fixed);
            } catch (const json::exception& e2) {
                spdlog::warn("XmlToolCallExtractor: invalid Qwen JSON after repair "
                             "(orig: {}; repair: {}): {}",
                             e.what(), e2.what(), trimmed.substr(0, 120));
                return;
            }
        } else {
            spdlog::warn("XmlToolCallExtractor: invalid Qwen JSON ({}): {}",
                         e.what(), trimmed.substr(0, 120));
            return;
        }
    }

    if (!j.is_object() || !j.contains("name")) {
        spdlog::warn("XmlToolCallExtractor: Qwen body missing 'name': {}",
                     trimmed.substr(0, 120));
        return;
    }

    StreamDecoderSink::ToolCallDelta d;
    d.index     = next_index;
    d.id_frag   = synth_id(next_index);
    d.name_frag = j.value("name", "");

    if (j.contains("arguments")) {
        auto& a = j["arguments"];
        if (a.is_string())      d.args_frag = a.get<std::string>();
        else if (a.is_object()) d.args_frag = a.dump();
        else if (a.is_null())   d.args_frag = "{}";
        else                    d.args_frag = a.dump();
    } else {
        d.args_frag = "{}";
    }

    if (on_tool_call) on_tool_call(d);
    ++next_index;
}

// ----- Claude body parsing ---------------------------------------------------

namespace {

// Scan a Claude <function_calls> body for individual <invoke> blocks.
// Returns each block's name + map of parameter-name -> raw text body.
struct ClaudeInvoke {
    std::string                                       name;
    std::vector<std::pair<std::string, std::string>>  params;
};

std::vector<ClaudeInvoke> parse_claude_invokes(const std::string& body)
{
    std::vector<ClaudeInvoke> out;

    size_t i = 0;
    for (;;) {
        size_t open = body.find("<invoke", i);
        if (open == std::string::npos) break;

        size_t name_attr = body.find("name=\"", open);
        if (name_attr == std::string::npos) break;
        name_attr += 6;
        size_t name_end = body.find('"', name_attr);
        if (name_end == std::string::npos) break;

        size_t open_close = body.find('>', name_end);
        if (open_close == std::string::npos) break;

        size_t close = body.find("</invoke>", open_close);
        if (close == std::string::npos) break;

        ClaudeInvoke inv;
        inv.name = body.substr(name_attr, name_end - name_attr);

        // Walk parameters inside this invoke.
        size_t p = open_close + 1;
        while (p < close) {
            size_t p_open = body.find("<parameter", p);
            if (p_open == std::string::npos || p_open > close) break;

            size_t p_name = body.find("name=\"", p_open);
            if (p_name == std::string::npos || p_name > close) break;
            p_name += 6;
            size_t p_name_end = body.find('"', p_name);
            if (p_name_end == std::string::npos || p_name_end > close) break;

            size_t p_open_close = body.find('>', p_name_end);
            if (p_open_close == std::string::npos || p_open_close > close) break;

            size_t p_close = body.find("</parameter>", p_open_close);
            if (p_close == std::string::npos || p_close > close) break;

            std::string pname = body.substr(p_name, p_name_end - p_name);
            std::string pval  = body.substr(p_open_close + 1,
                                            p_close - (p_open_close + 1));
            inv.params.push_back({pname, decode_entities(pval)});
            p = p_close + std::strlen("</parameter>");
        }

        out.push_back(std::move(inv));
        i = close + std::strlen("</invoke>");
    }
    return out;
}

// Convert a parameter-text value to a JSON value. Models emit raw text
// inside <parameter>, so a path like "src/x.cpp" should land as a JSON
// string. But integers / booleans / arrays should round-trip as their
// natural types. Heuristic: try to parse as JSON; on failure, keep the
// trimmed text as a string. Empty-string stays empty-string.
json value_from_param_text(const std::string& raw)
{
    std::string t = trim(raw);
    if (t.empty()) return std::string();

    // Common scalars first -- avoids json::parse complaining about bare words.
    if (t == "true")  return true;
    if (t == "false") return false;
    if (t == "null")  return nullptr;

    // Numbers, JSON objects, JSON arrays.
    if ((t.front() >= '0' && t.front() <= '9') ||
        t.front() == '-' || t.front() == '{' || t.front() == '[' ||
        t.front() == '"') {
        try {
            return json::parse(t);
        } catch (...) { /* fall through */ }
    }

    // Default: raw string, preserving original (untrimmed) value so
    // multi-line text bodies don't lose formatting.
    return raw;
}

} // namespace

void XmlToolCallExtractor::parse_claude_body(const std::string& body,
                                             int& next_index,
                                             const OnToolCall& on_tool_call)
{
    auto invokes = parse_claude_invokes(body);
    if (invokes.empty()) {
        spdlog::warn("XmlToolCallExtractor: no <invoke> blocks in <function_calls> body");
        return;
    }

    for (auto& inv : invokes) {
        json args = json::object();
        for (auto& [k, v] : inv.params)
            args[k] = value_from_param_text(v);

        StreamDecoderSink::ToolCallDelta d;
        d.index     = next_index;
        d.id_frag   = synth_id(next_index);
        d.name_frag = inv.name;
        d.args_frag = args.dump();

        if (on_tool_call) on_tool_call(d);
        ++next_index;
    }
}

// ----- Hermes / GLM body parsing --------------------------------------------

namespace {

// Read a function name out of an opening "<function..." tag. Accepts both
//   <function=NAME>
// and
//   <function name="NAME">
// Returns (name, byte after the closing '>') or {{}, npos} on parse failure.
// `start` must point at the '<' of "<function".
std::pair<std::string, size_t> read_hermes_tag_name(const std::string& s,
                                                    size_t start,
                                                    const char* tag /*"function" or "parameter"*/)
{
    size_t tag_len = std::strlen(tag);
    // s[start] == '<', followed by tag.
    size_t i = start + 1 + tag_len;   // past '<' + tag
    if (i > s.size()) return {{}, std::string::npos};

    std::string name;
    if (i < s.size() && s[i] == '=') {
        // `=NAME>` form -- name is everything up to the next '>'.
        size_t close = s.find('>', i + 1);
        if (close == std::string::npos) return {{}, std::string::npos};
        name = s.substr(i + 1, close - (i + 1));
        // Strip surrounding whitespace / quotes the model might emit.
        name = trim(name);
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
            name = name.substr(1, name.size() - 2);
        return {name, close + 1};
    }

    // Attribute form: `<function name="NAME">`. Look for `name="..."` up to '>'.
    size_t close = s.find('>', i);
    if (close == std::string::npos) return {{}, std::string::npos};
    size_t attr = s.find("name=\"", i);
    if (attr == std::string::npos || attr > close) return {{}, std::string::npos};
    attr += 6;   // past name="
    size_t attr_end = s.find('"', attr);
    if (attr_end == std::string::npos || attr_end > close)
        return {{}, std::string::npos};
    name = s.substr(attr, attr_end - attr);
    return {name, close + 1};
}

} // namespace

void XmlToolCallExtractor::parse_hermes_body(const std::string& body,
                                             int& next_index,
                                             const OnToolCall& on_tool_call)
{
    // Walk every <function...>...</function> block. Most models emit only
    // one per <tool_call>, but tolerate multiples for symmetry with Claude.
    size_t i = 0;
    int emitted_here = 0;
    for (;;) {
        size_t f_open = body.find("<function", i);
        if (f_open == std::string::npos) break;

        auto [fn_name, after_open] = read_hermes_tag_name(body, f_open, "function");
        if (after_open == std::string::npos || fn_name.empty()) {
            spdlog::warn("XmlToolCallExtractor: malformed Hermes <function> tag");
            return;
        }

        size_t f_close = body.find("</function>", after_open);
        if (f_close == std::string::npos) {
            spdlog::warn("XmlToolCallExtractor: Hermes <function> missing close");
            return;
        }

        json args = json::object();
        size_t p = after_open;
        while (p < f_close) {
            size_t p_open = body.find("<parameter", p);
            if (p_open == std::string::npos || p_open >= f_close) break;

            auto [p_name, after_p_open] =
                read_hermes_tag_name(body, p_open, "parameter");
            if (after_p_open == std::string::npos || p_name.empty()) {
                spdlog::warn("XmlToolCallExtractor: malformed Hermes <parameter> tag");
                break;
            }

            size_t p_close = body.find("</parameter>", after_p_open);
            if (p_close == std::string::npos || p_close > f_close) break;

            std::string raw = body.substr(after_p_open, p_close - after_p_open);
            // Models commonly put the value on its own line:
            //   <parameter=path>\n  src/x.cpp\n</parameter>
            // Trim so the value lands as expected; value_from_param_text
            // also trims internally, but doing it here keeps the raw-string
            // fallback (multi-line bodies) from carrying stray newlines.
            args[p_name] = value_from_param_text(trim(raw));
            p = p_close + std::strlen("</parameter>");
        }

        StreamDecoderSink::ToolCallDelta d;
        d.index     = next_index;
        d.id_frag   = synth_id(next_index);
        d.name_frag = fn_name;
        d.args_frag = args.dump();
        if (on_tool_call) on_tool_call(d);
        ++next_index;
        ++emitted_here;

        i = f_close + std::strlen("</function>");
    }

    if (emitted_here == 0)
        spdlog::warn("XmlToolCallExtractor: no <function> blocks in Hermes body");
}

} // namespace locus
