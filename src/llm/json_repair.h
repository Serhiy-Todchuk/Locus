#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace locus {

// S6.10 Task A -- JSON repair pre-pass for malformed tool-call bodies.
//
// Small models emit malformed JSON in tool-call arguments more often than
// frontier models: trailing commas, unquoted keys, single-quoted strings,
// missing closing braces, literal (raw) newlines inside string values, or
// extra surrounding prose around a valid JSON object. Each stage targets
// one of those failure modes; the helper runs all stages in sequence and
// returns the repaired text if it parses cleanly afterwards.
//
// Pure function. Designed to run on the hot path between
// `nlohmann::json::parse` raising on a tool-call body and the caller
// dropping the call entirely. When repair fails the helper returns
// `nullopt` and the caller falls back to its existing drop-the-call path.

struct JsonRepairResult {
    std::string fixed;           // repaired text (parses cleanly)
    std::string stages_applied;  // pipe-separated diagnostic, e.g.
                                 //   "trailing_comma|unquoted_keys|balance"
                                 // empty when no stage fired (input was
                                 // already parseable).
};

// Apply every repair stage in order. Returns `nullopt` only when even the
// repaired text fails to parse as JSON -- callers should drop the call in
// that case. Empty input is also `nullopt` (nothing to repair).
//
// The returned text IS guaranteed to parse with `nlohmann::json::parse`,
// so callers may skip a second parsability check, but they will still need
// to parse to bind the values.
std::optional<JsonRepairResult> repair_for_parse(std::string_view input);

}  // namespace locus
