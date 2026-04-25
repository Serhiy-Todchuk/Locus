#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace locus::eval {

namespace fs = std::filesystem;

// One search query extracted from a saved session's tool calls.
struct CuratedQuery {
    std::string source_session;  // Session ID (filename stem)
    std::string mode;             // "text" / "regex" / "semantic" / "hybrid" / "symbols"
    std::string query;            // Raw query text from the tool args
    std::string pattern;          // For regex mode (else empty)
};

// Walk a sessions directory (.locus/sessions/*.json), parse each session, and
// extract every search-tool invocation from assistant messages. Tool args are
// stored as raw JSON strings on each ChatMessage::tool_calls entry — we parse
// them here to recover the user-facing query text.
//
// Designed as the "activity-log replay" hook from S4.K: once a real user has
// driven the agent for a while, drop the harvested list into queries.json and
// hand-add expected_files to turn them into gold-standard queries.
//
// `sessions_dir` need not exist — returns empty on missing dir.
std::vector<CuratedQuery>
curate_from_sessions(const fs::path& sessions_dir);

// Render a list of curated queries as a JSON template that mirrors the
// queries.json schema (`expected_files` left as []). The caller pipes this to
// stdout / a file to bootstrap new gold queries.
std::string
to_queries_json_template(const std::vector<CuratedQuery>& queries);

} // namespace locus::eval
