#include "tools/process_output_filter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <regex>
#include <sstream>
#include <vector>

namespace locus {

namespace {

// Split `text` into lines tolerating CRLF / lone CR / LF so a Windows
// build log doesn't smear into one giant line on the head/tail path.
std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            out.push_back(std::move(current));
            current.clear();
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

std::string join_lines(const std::vector<std::string>& lines,
                       int from, int to_exclusive)
{
    std::ostringstream o;
    for (int i = from; i < to_exclusive; ++i) {
        o << lines[i];
        if (i + 1 < to_exclusive) o << '\n';
    }
    return o.str();
}

std::string elide_marker(int elided)
{
    std::ostringstream o;
    o << "[... " << elided
      << " lines elided; full output in .locus/locus.log (trace level) ...]";
    return o.str();
}

std::string head_only(const std::string& original,
                       const std::vector<std::string>& lines, int keep)
{
    const int total = static_cast<int>(lines.size());
    if (keep >= total) return original;  // no-op: preserves original trailing-NL
    std::string body = join_lines(lines, 0, keep);
    body += "\n";
    body += elide_marker(total - keep);
    return body;
}

std::string tail_only(const std::string& original,
                       const std::vector<std::string>& lines, int keep)
{
    const int total = static_cast<int>(lines.size());
    if (keep >= total) return original;
    std::string body = elide_marker(total - keep);
    body += "\n";
    body += join_lines(lines, total - keep, total);
    return body;
}

std::string head_tail(const std::string& original,
                      const std::vector<std::string>& lines, int per_side)
{
    const int total = static_cast<int>(lines.size());
    if (per_side <= 0 || total <= per_side * 2) return original;
    std::string body = join_lines(lines, 0, per_side);
    body += "\n";
    body += elide_marker(total - per_side * 2);
    body += "\n";
    body += join_lines(lines, total - per_side, total);
    return body;
}

std::string apply_regex(const std::vector<std::string>& lines,
                        const std::string& pattern,
                        bool case_sensitive,
                        bool is_regex,
                        int max_matches,
                        int context)
{
    const int total = static_cast<int>(lines.size());

    // Build a per-line matcher. For substring mode we lowercase needle
    // once + haystack on the fly when case_sensitive is false.
    std::function<bool(const std::string&)> matches;
    if (is_regex) {
        std::regex_constants::syntax_option_type flags = std::regex::ECMAScript;
        if (!case_sensitive) flags |= std::regex::icase;
        try {
            auto re = std::make_shared<std::regex>(pattern, flags);
            matches = [re](const std::string& s) {
                return std::regex_search(s, *re);
            };
        } catch (const std::regex_error& e) {
            // Bubble the error up via an elision marker -- callers feed the
            // body straight to the LLM, so we want a self-explanatory line.
            std::ostringstream err;
            err << "[output_filter: invalid regex '" << pattern << "': "
                << e.what() << "]";
            return err.str();
        }
    } else {
        std::string needle = pattern;
        if (!case_sensitive)
            std::transform(needle.begin(), needle.end(), needle.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        matches = [needle, case_sensitive](const std::string& s) {
            if (case_sensitive) return s.find(needle) != std::string::npos;
            std::string hay(s.size(), 0);
            std::transform(s.begin(), s.end(), hay.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return hay.find(needle) != std::string::npos;
        };
    }

    // First pass -- which lines match.
    std::vector<bool> hit(lines.size(), false);
    int total_matches = 0;
    for (int i = 0; i < total; ++i) {
        if (matches(lines[i])) { hit[i] = true; ++total_matches; }
    }

    // Second pass -- materialise up to max_matches blocks with context.
    std::vector<bool> emitted(lines.size(), false);
    std::vector<int>  order;
    order.reserve(static_cast<std::size_t>(
        std::min(total_matches, max_matches)) * (1 + context * 2));
    int kept = 0;
    for (int i = 0; i < total && kept < max_matches; ++i) {
        if (!hit[i]) continue;
        int lo = std::max(0, i - context);
        int hi = std::min(total - 1, i + context);
        for (int j = lo; j <= hi; ++j) {
            if (emitted[j]) continue;
            emitted[j] = true;
            order.push_back(j);
        }
        ++kept;
    }

    std::ostringstream summary;
    summary << "[output_filter " << (is_regex ? "regex" : "substring")
            << ": " << total_matches << " of " << total
            << " lines matched";
    if (kept < total_matches)
        summary << ", capped at " << max_matches;
    summary << "]";

    if (order.empty()) return summary.str();

    std::ostringstream body;
    body << summary.str() << '\n';
    int last_idx = -2;
    for (int idx : order) {
        if (last_idx >= 0 && idx != last_idx + 1) body << "--\n";
        char sep = hit[idx] ? ':' : '-';
        body << (idx + 1) << sep << lines[idx] << '\n';
        last_idx = idx;
    }
    return body.str();
}

} // namespace

std::string apply_output_filter(const std::string& text,
                                const OutputFilterSpec& spec,
                                int default_truncate_lines)
{
    // Empty input -> nothing to filter (also covers the "command produced no
    // stdout" common case in run_command).
    if (text.empty()) return text;

    auto lines = split_lines(text);
    const int total = static_cast<int>(lines.size());

    // Effective per-side / max-matches count: spec.lines wins if positive,
    // otherwise fall back to the workspace default. A default of 0 means
    // "no smart-truncate" -- only explicit filters do anything.
    int lines_n = spec.lines > 0 ? spec.lines : default_truncate_lines;

    const std::string& mode = spec.mode;
    if (mode.empty() || mode == "head_tail") {
        if (lines_n <= 0) return text;
        return head_tail(text, lines, lines_n);
    }
    if (mode == "head") {
        if (lines_n <= 0) lines_n = total;
        return head_only(text, lines, lines_n);
    }
    if (mode == "tail") {
        if (lines_n <= 0) lines_n = total;
        return tail_only(text, lines, lines_n);
    }
    if (mode == "regex" || mode == "substring") {
        int max_matches = lines_n > 0 ? lines_n : 50;
        int ctx = std::clamp(spec.context, 0, 10);
        if (spec.pattern.empty()) {
            return "[output_filter: pattern is required for mode '" + mode + "']";
        }
        return apply_regex(lines, spec.pattern, spec.case_sensitive,
                           mode == "regex", max_matches, ctx);
    }
    return "[output_filter: unknown mode '" + mode + "']";
}

bool parse_output_filter_args(const nlohmann::json& args,
                              OutputFilterSpec& out_spec,
                              std::string& out_error)
{
    out_spec = OutputFilterSpec{};

    if (args.contains("output_filter_mode")) {
        if (!args["output_filter_mode"].is_string()) {
            out_error = "output_filter_mode must be a string";
            return false;
        }
        std::string m = args["output_filter_mode"].get<std::string>();
        if (!m.empty() && m != "head" && m != "tail" && m != "head_tail" &&
            m != "regex" && m != "substring") {
            out_error = "output_filter_mode must be one of "
                        "'head', 'tail', 'head_tail', 'regex', 'substring'";
            return false;
        }
        out_spec.mode = std::move(m);
    }
    if (args.contains("output_filter_pattern")) {
        if (!args["output_filter_pattern"].is_string()) {
            out_error = "output_filter_pattern must be a string";
            return false;
        }
        out_spec.pattern = args["output_filter_pattern"].get<std::string>();
    }
    if (args.contains("output_filter_lines")) {
        if (!args["output_filter_lines"].is_number_integer()) {
            out_error = "output_filter_lines must be an integer";
            return false;
        }
        int n = args["output_filter_lines"].get<int>();
        if (n < 0) n = 0;
        out_spec.lines = n;
    }
    if (args.contains("output_filter_context")) {
        if (!args["output_filter_context"].is_number_integer()) {
            out_error = "output_filter_context must be an integer";
            return false;
        }
        int c = args["output_filter_context"].get<int>();
        if (c < 0) c = 0;
        if (c > 10) c = 10;
        out_spec.context = c;
    }
    if (args.contains("output_filter_case_sensitive")) {
        if (!args["output_filter_case_sensitive"].is_boolean()) {
            out_error = "output_filter_case_sensitive must be a boolean";
            return false;
        }
        out_spec.case_sensitive =
            args["output_filter_case_sensitive"].get<bool>();
    }
    return true;
}

} // namespace locus
