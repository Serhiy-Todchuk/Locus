#include "filter_output_tool.h"

#include "shared.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <regex>
#include <sstream>
#include <vector>

namespace locus {

using tools::error_result;

namespace {

// Split `text` into lines without copying the original buffer. Trailing
// newline (if any) does not produce a phantom empty line. CRLF / LF /
// bare-CR are all tolerated -- CRLF / lone CR collapses into a normal
// line break so a Windows build log doesn't smear into a single line.
std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            out.push_back(std::move(current));
            current.clear();
            // skip a paired \n so CRLF -> one line
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

} // namespace

std::string FilterOutputTool::preview(const ToolCall& call) const
{
    std::string mode    = call.args.value("mode", "regex");
    std::string pattern = call.args.value("pattern", "");
    if (pattern.size() > 60) pattern = pattern.substr(0, 57) + "...";
    std::string out = "filter_output [" + mode + "]";
    if (!pattern.empty()) out += ": " + pattern;
    if (call.args.value("invert", false)) out += "  (invert)";
    return out;
}

ToolResult FilterOutputTool::execute(const ToolCall& call,
                                      IWorkspaceServices& /*ws*/,
                                      const std::atomic<bool>* /*cancel_flag*/)
{
    if (!call.args.contains("text") || !call.args["text"].is_string())
        return error_result("Error: 'text' parameter is required (string)");
    if (!call.args.contains("pattern") || !call.args["pattern"].is_string())
        return error_result("Error: 'pattern' parameter is required (string)");

    std::string text    = call.args["text"].get<std::string>();
    std::string pattern = call.args["pattern"].get<std::string>();
    std::string mode    = call.args.value("mode", std::string{"regex"});
    bool case_sensitive = call.args.value("case_sensitive", false);
    int max_matches     = call.args.value("max_matches", 50);
    int before          = call.args.value("before", 0);
    int after           = call.args.value("after", 0);
    bool invert         = call.args.value("invert", false);

    if (mode != "regex" && mode != "substring")
        return error_result("Error: 'mode' must be 'regex' or 'substring'");
    if (max_matches <= 0) max_matches = 50;
    if (before < 0) before = 0;
    if (after  < 0) after  = 0;
    if (before > 10) before = 10;
    if (after  > 10) after  = 10;
    if (pattern.empty())
        return error_result("Error: 'pattern' must be non-empty");

    auto lines = split_lines(text);
    const int total_lines = static_cast<int>(lines.size());

    // Build a matcher closure that handles both modes + case sensitivity in
    // one place. Regex compile is the only thing that can fail at runtime,
    // so we surface a clean error message instead of letting std::regex_error
    // bubble up.
    std::function<bool(const std::string&)> matches;
    if (mode == "regex") {
        std::regex_constants::syntax_option_type flags = std::regex::ECMAScript;
        if (!case_sensitive) flags |= std::regex::icase;
        try {
            auto re = std::make_shared<std::regex>(pattern, flags);
            matches = [re](const std::string& s) {
                return std::regex_search(s, *re);
            };
        } catch (const std::regex_error& e) {
            return error_result(std::string("Error: invalid regex '") + pattern +
                                "': " + e.what());
        }
    } else {
        // substring
        std::string needle = pattern;
        if (!case_sensitive) {
            std::transform(needle.begin(), needle.end(), needle.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        }
        matches = [needle, case_sensitive](const std::string& s) {
            if (case_sensitive) return s.find(needle) != std::string::npos;
            std::string hay(s.size(), 0);
            std::transform(s.begin(), s.end(), hay.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return hay.find(needle) != std::string::npos;
        };
    }

    // First pass -- count total matches without producing output (so the
    // summary can report "X of Y" even when we cap the response).
    int total_matches = 0;
    std::vector<bool> hit(lines.size(), false);
    for (int i = 0; i < total_lines; ++i) {
        bool m = matches(lines[i]);
        if (invert) m = !m;
        if (m) {
            hit[i] = true;
            ++total_matches;
        }
    }

    // Second pass -- materialise matching blocks with context, capped by
    // max_matches. Each kept hit also pulls in `before` lines above and
    // `after` lines below; overlapping context dedups via a per-line
    // "already emitted" set so the output stays readable.
    std::vector<int> emitted;
    emitted.reserve(static_cast<std::size_t>(std::min(total_matches, max_matches)) *
                    (1 + before + after));
    std::vector<bool> seen(lines.size(), false);

    int kept = 0;
    for (int i = 0; i < total_lines && kept < max_matches; ++i) {
        if (!hit[i]) continue;
        int lo = std::max(0, i - before);
        int hi = std::min(total_lines - 1, i + after);
        for (int j = lo; j <= hi; ++j) {
            if (seen[j]) continue;
            seen[j] = true;
            emitted.push_back(j);
        }
        ++kept;
    }

    std::ostringstream body;
    // Each emitted line is prefixed with its 1-based line number plus a
    // separator that distinguishes match (`:`) from context (`-`), matching
    // grep -n / grep -C output. Adjacent non-contiguous blocks are split by
    // a `--` separator line.
    int last_idx = -2;
    for (int idx : emitted) {
        if (last_idx >= 0 && idx != last_idx + 1)
            body << "--\n";
        char sep = hit[idx] ? ':' : '-';
        body << (idx + 1) << sep << lines[idx] << '\n';
        last_idx = idx;
    }

    std::ostringstream summary;
    summary << total_matches << " of " << total_lines << " lines matched";
    if (kept < total_matches)
        summary << " (capped at " << max_matches << ")";

    std::string out;
    out += summary.str() + "\n";
    if (!emitted.empty()) {
        out += "\n";
        out += body.str();
    }

    spdlog::trace("filter_output: pattern='{}' mode={} matches={}/{} returned={}",
                  pattern, mode, total_matches, total_lines, kept);

    return {true, out, out};
}

} // namespace locus
