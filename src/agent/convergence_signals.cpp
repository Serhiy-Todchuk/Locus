#include "agent/convergence_signals.h"

#include <algorithm>
#include <cctype>

namespace locus {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

} // namespace

// ---- Task 1 ----------------------------------------------------------------

AllDroppedAction classify_all_dropped(int delivered, int dropped,
                                      bool already_nudged)
{
    // A round with at least one delivered call, or with no tool calls at all,
    // is not the all-dropped loop. The caller resets the streak on `none`.
    if (delivered > 0) return AllDroppedAction::none;
    if (dropped <= 0)  return AllDroppedAction::none;

    // delivered == 0 && dropped >= 1: every tool call this round was malformed.
    return already_nudged ? AllDroppedAction::abort
                          : AllDroppedAction::nudge;
}

// ---- Task 2 ----------------------------------------------------------------

bool is_edit_ambiguity_error(const std::string& tool_output)
{
    std::string low = lower(tool_output);
    // Only an actual error body counts. Both branches carry "'old_string'".
    if (!contains(low, "'old_string'")) return false;

    // Ambiguous-match case: "'old_string' matches N locations ...".
    if (contains(low, "matches") && contains(low, "location"))
        return true;

    // Not-found case: "'old_string' not found in ... (exact match required ...".
    if (contains(low, "not found"))
        return true;

    return false;
}

// ---- Task 3 ----------------------------------------------------------------

bool claims_unverified_success(
    const std::string& assistant_text,
    const std::vector<std::string>& this_round_tool_names)
{
    if (assistant_text.empty()) return false;

    // Any tool that could ground a build/run claim disqualifies the round.
    // Conservative: if the model verified ANYTHING this round we stay silent.
    for (const auto& name : this_round_tool_names) {
        std::string n = lower(name);
        if (n == "run_command"     || n == "run_command_bg"
         || n == "read_process_output"
         || n == "read_file"       || n == "list_directory"
         || n == "get_file_outline"
         || n.rfind("search_", 0) == 0)   // search_text / search_semantic / ...
            return false;
    }

    std::string low = lower(assistant_text);

    // Curated success-claim phrases. Deliberately specific: "builds" /
    // "compiles" / a produced-artifact phrase. Generic words like "done",
    // "fixed", "should work" are excluded to avoid flagging honest summaries.
    static const char* k_phrases[] = {
        "build succeeds",      "build now succeeds",  "builds successfully",
        "builds cleanly",      "now builds",          "the build passes",
        "compiles successfully", "compiles cleanly",  "now compiles",
        "compiled successfully",
        "build is successful", "build succeeded",
        "the exe runs",        "executable runs",     "the program runs",
        "runs successfully",   "ran successfully",
        "frame0",              ".ppm",                ".png was created",
        "screenshot created",  "screenshot was created",
        "produced frame",      "rendered the frame",
        "artifact was created", "the binary was produced",
    };
    for (const char* p : k_phrases) {
        if (contains(low, p)) return true;
    }
    return false;
}

} // namespace locus
