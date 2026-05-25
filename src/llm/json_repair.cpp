#include "llm/json_repair.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace locus {

namespace {

// Forward-walking helper that tracks whether the current index is INSIDE a
// double-quoted JSON string. JSON does not allow line breaks inside strings,
// so `\n` / `\r` inside a string are themselves a syntax violation -- those
// are explicitly NOT treated as string terminators (stage 5 fixes them).
//
// Returns the value of `in_string` AFTER processing s[i].
//
// `i` must be a valid index in s.
struct StringScanState {
    bool in_string = false;   // currently inside a "..." span
    bool escape    = false;   // previous byte was a backslash inside a string
};

void step_string_state(char c, StringScanState& st)
{
    if (st.in_string) {
        if (st.escape) {
            st.escape = false;
        } else if (c == '\\') {
            st.escape = true;
        } else if (c == '"') {
            st.in_string = false;
        }
    } else {
        if (c == '"') {
            st.in_string = true;
        }
    }
}

bool parses_cleanly(const std::string& text)
{
    try {
        (void) nlohmann::json::parse(text);
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Stage 1 -- strip trailing commas before `}` or `]`.
// Walk the buffer once, keeping string-state in sync. Outside a string,
// if we land on a comma and the next non-whitespace byte is `}` or `]`,
// drop the comma. Multiple trailing commas (e.g. `,,}`) get peeled in one
// pass because the rewritten text has one less comma each time we encounter
// the pattern.
// ---------------------------------------------------------------------------
std::string strip_trailing_commas(const std::string& in, bool& fired)
{
    std::string out;
    out.reserve(in.size());
    StringScanState st;

    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];

        if (!st.in_string && c == ',') {
            // Peek ahead past whitespace.
            size_t j = i + 1;
            while (j < in.size() &&
                   (in[j] == ' ' || in[j] == '\t' ||
                    in[j] == '\n' || in[j] == '\r'))
                ++j;
            if (j < in.size() && (in[j] == '}' || in[j] == ']')) {
                fired = true;
                continue;  // drop this comma
            }
        }

        out.push_back(c);
        step_string_state(c, st);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Stage 2 -- wrap unquoted identifier keys.
// `{foo: 1}` -> `{"foo": 1}`. Only fires at object-key positions: directly
// after `{` or `,` (with optional whitespace), followed by an identifier
// (`[A-Za-z_][A-Za-z0-9_-]*`), optional whitespace, then `:`. String-state
// aware: inside a string we never touch anything.
// ---------------------------------------------------------------------------
bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool is_ident_cont (char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; }

std::string quote_unquoted_keys(const std::string& in, bool& fired)
{
    std::string out;
    out.reserve(in.size() + 16);
    StringScanState st;

    auto can_start_key = [&](size_t pos) -> bool {
        // Walk back over whitespace to find the preceding non-ws byte.
        size_t k = pos;
        while (k > 0 &&
               (out[k - 1] == ' ' || out[k - 1] == '\t' ||
                out[k - 1] == '\n' || out[k - 1] == '\r'))
            --k;
        if (k == 0) return false;  // no anchor in already-emitted text
        char prev = out[k - 1];
        return prev == '{' || prev == ',';
    };

    for (size_t i = 0; i < in.size(); ) {
        char c = in[i];

        if (!st.in_string && is_ident_start(c) && can_start_key(out.size())) {
            // Try to consume an identifier.
            size_t j = i + 1;
            while (j < in.size() && is_ident_cont(in[j])) ++j;

            // Skip whitespace after the candidate identifier.
            size_t k = j;
            while (k < in.size() &&
                   (in[k] == ' ' || in[k] == '\t' ||
                    in[k] == '\n' || in[k] == '\r'))
                ++k;

            if (k < in.size() && in[k] == ':') {
                // Found `<ident> [ws]* :` at an object-key position.
                fired = true;
                out.push_back('"');
                out.append(in, i, j - i);
                out.push_back('"');
                // Don't update string-state from the inserted quotes -- we
                // emitted a balanced pair, net state is the same as before.
                i = j;
                continue;
            }
        }

        out.push_back(c);
        step_string_state(c, st);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Stage 3 -- convert single-quoted strings to double-quoted.
// Only fires when no double quotes appear in the input AT ALL. The intent is
// to convert "Python-style" JSON like `{'k': 'v'}`; if double quotes are
// already present we'd risk corrupting an embedded `"don't"` string by
// rewriting its quote.
// ---------------------------------------------------------------------------
std::string single_to_double_quotes(const std::string& in, bool& fired)
{
    if (in.find('"') != std::string::npos) return in;
    if (in.find('\'') == std::string::npos) return in;

    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\'') { out.push_back('"'); fired = true; }
        else            out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Stage 4 -- patch missing closing braces / brackets.
// Walk once, counting `{}` and `[]` while outside strings. If a positive
// difference remains at end-of-input, append closers. Tracks the ORDER of
// opens via a stack so we close in the right shape (e.g. `{"a":[1` -> close
// `]` then `}`).
// ---------------------------------------------------------------------------
std::string patch_balance(const std::string& in, bool& fired)
{
    std::vector<char> stack;
    StringScanState st;

    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];

        if (!st.in_string) {
            if (c == '{')      stack.push_back('}');
            else if (c == '[') stack.push_back(']');
            else if (c == '}' || c == ']') {
                if (!stack.empty() && stack.back() == c) stack.pop_back();
                // Mismatched closers are left alone -- not our job to
                // rebalance pathological input; let json::parse complain.
            }
        }
        step_string_state(c, st);
    }

    // Also close an unterminated string if we end mid-string. Doing this
    // before adding bracket closers means the closers land outside the
    // resurrected string, which is what we want.
    std::string out = in;
    if (st.in_string) {
        out.push_back('"');
        fired = true;
    }
    while (!stack.empty()) {
        out.push_back(stack.back());
        stack.pop_back();
        fired = true;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Stage 5 -- escape literal newlines / tabs / carriage returns inside
// string values. A raw \n byte inside a JSON string is a syntax error
// (RFC 8259 §7 says control chars must be \-escaped). Models that emit
// multi-line code in a tool arg routinely break this rule. Walk strings
// and rewrite the offending bytes.
// ---------------------------------------------------------------------------
std::string escape_literal_controls(const std::string& in, bool& fired)
{
    std::string out;
    out.reserve(in.size());
    StringScanState st;

    for (char c : in) {
        if (st.in_string) {
            if (c == '\n')      { out.append("\\n");  fired = true; continue; }
            if (c == '\r')      { out.append("\\r");  fired = true; continue; }
            if (c == '\t')      { out.append("\\t");  fired = true; continue; }
        }
        out.push_back(c);
        step_string_state(c, st);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Stage 6 (fallback) -- extract the first balanced {...} block from a longer
// string. For inputs like:
//   Sure, here is the call: {"name": "x", "args": {...}} hope that helps
// extract just the object body. String-state aware so braces inside string
// literals don't throw off the count. Returns nullopt when no balanced
// object can be found.
// ---------------------------------------------------------------------------
std::optional<std::string> extract_first_balanced(const std::string& in)
{
    // Find first '{' outside any string. Strings starting before the first
    // '{' are unusual but tolerated -- if a `"` precedes the brace, we step
    // through the string properly so its contents don't disturb the count.
    StringScanState st;
    size_t start = std::string::npos;
    for (size_t i = 0; i < in.size(); ++i) {
        if (!st.in_string && in[i] == '{') {
            start = i;
            break;
        }
        step_string_state(in[i], st);
    }
    if (start == std::string::npos) return std::nullopt;

    // Walk from the opening brace forward, matching depth.
    int depth = 0;
    StringScanState st2;
    for (size_t i = start; i < in.size(); ++i) {
        char c = in[i];
        if (!st2.in_string) {
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0)
                    return in.substr(start, i - start + 1);
            }
        }
        step_string_state(c, st2);
    }
    return std::nullopt;  // unbalanced -- let stage 4 try at the caller
}

void append_stage(std::string& list, const char* name)
{
    if (!list.empty()) list.push_back('|');
    list.append(name);
}

// Run stages 1..5 in order, accumulating the diagnostic list.
std::string run_inner_stages(const std::string& in, std::string& diagnostic)
{
    std::string text = in;
    bool fired = false;

    fired = false;
    text = strip_trailing_commas(text, fired);
    if (fired) append_stage(diagnostic, "trailing_comma");

    fired = false;
    text = quote_unquoted_keys(text, fired);
    if (fired) append_stage(diagnostic, "unquoted_keys");

    fired = false;
    text = single_to_double_quotes(text, fired);
    if (fired) append_stage(diagnostic, "single_quotes");

    fired = false;
    text = escape_literal_controls(text, fired);
    if (fired) append_stage(diagnostic, "literal_controls");

    fired = false;
    text = patch_balance(text, fired);
    if (fired) append_stage(diagnostic, "balance");

    return text;
}

}  // namespace

std::optional<JsonRepairResult> repair_for_parse(std::string_view input)
{
    if (input.empty()) return std::nullopt;

    std::string original(input);

    // Fast path: already parses cleanly.
    if (parses_cleanly(original)) {
        return JsonRepairResult{original, std::string{}};
    }

    // Stages 1..5 on the original.
    std::string diag;
    std::string repaired = run_inner_stages(original, diag);
    if (parses_cleanly(repaired)) {
        return JsonRepairResult{std::move(repaired), std::move(diag)};
    }

    // Fallback: extract first balanced {...} from the (already-stage-balanced)
    // text and re-run stages 1..5 on the extracted block. Using the post-
    // stage-4 text means a half-closed object like `prose {"a":1` benefits
    // from the appended `}` before extraction.
    if (auto extracted = extract_first_balanced(repaired)) {
        append_stage(diag, "extract_balanced");
        std::string inner_diag;
        std::string repaired2 = run_inner_stages(*extracted, inner_diag);
        if (!inner_diag.empty()) {
            // Re-applied stages on the trimmed window -- record so the caller
            // can see the full chain.
            append_stage(diag, ("rerun:" + inner_diag).c_str());
        }
        if (parses_cleanly(repaired2)) {
            return JsonRepairResult{std::move(repaired2), std::move(diag)};
        }
    }

    return std::nullopt;
}

}  // namespace locus
