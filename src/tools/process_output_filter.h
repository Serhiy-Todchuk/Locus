#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace locus {

// Output filter applied to the body of run_command / read_process_output
// before the result enters the LLM's tool message. The full raw output is
// still spdlog::trace'd by the caller so the user can recover it from
// `.locus/locus.log`; the LLM only ever sees the filtered slice.
//
// Modes:
//   ""           default. head_tail with `lines` = default_truncate_lines per
//                side. If `default_truncate_lines` <= 0 or the body fits, the
//                output is returned unchanged.
//   "head"       keep first `lines` lines; rest elided.
//   "tail"       keep last  `lines` lines; earlier elided.
//   "head_tail"  keep first AND last `lines` lines per side, with the middle
//                elided. No-op when the body is already short enough.
//   "regex"      keep lines matching `pattern` (ECMAScript). Up to `lines`
//                matches, with `context` lines of -B/-A around each match.
//   "substring"  same as regex but the pattern is a literal needle.
//
// When elision happens, a marker line of the form
//   [... K lines elided; full output in .locus/locus.log (trace level) ...]
// is inserted so the LLM knows the body was filtered.
struct OutputFilterSpec {
    std::string mode;       // "" / "head" / "tail" / "head_tail" / "regex" / "substring"
    std::string pattern;    // for regex / substring
    int  lines   = 0;       // per-side for head_tail; total for head/tail; max_matches for regex
    int  context = 0;       // 0..10 lines of context around each regex/substring hit
    bool case_sensitive = false;
};

// Apply the spec to `text`. `default_truncate_lines` supplies the value when
// `spec.lines == 0` and is also the "do nothing under this many lines"
// threshold when `spec.mode` is empty. Returns the filtered body.
std::string apply_output_filter(const std::string& text,
                                const OutputFilterSpec& spec,
                                int default_truncate_lines);

// Parse the four flat ToolCall args (`output_filter_mode`, `output_filter_pattern`,
// `output_filter_lines`, `output_filter_context`, plus optional
// `output_filter_case_sensitive`) into a spec. Returns false on validation
// failure with `out_error` set; on success returns true (out_spec populated;
// missing args leave default-constructed fields).
bool parse_output_filter_args(const nlohmann::json& args,
                              OutputFilterSpec& out_spec,
                              std::string& out_error);

} // namespace locus
