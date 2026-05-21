#pragma once

// S5.Z task 9 -- "Agentic testing" interactive driver.
//
// `locus_ui_tests.exe --agentic ...` runs Locus under UIA, then listens on a
// localhost TCP port for newline-delimited JSON requests describing the next
// op to run. A QA-LLM (typically Claude in another session) sends those ops,
// reads each JSON response, and decides what to do next. Use cases:
//
//   - Drive Locus through a real user scenario (e.g. "build a small C++
//     minigame") and observe how the local agent handles it under different
//     model configurations.
//   - Probe failure modes a deterministic script can't anticipate, since the
//     QA-LLM reacts to whatever Locus actually does.
//
// See tests/ui_automation/AGENTIC_TESTING.md for the wire protocol, op
// catalog, and recipes.

#include <filesystem>
#include <string>

namespace locus::uia {

struct AgenticOptions {
    std::filesystem::path locus_gui_path;
    std::filesystem::path workspace_dir;     // pre-resolved absolute path
    std::filesystem::path output_dir;        // pre-created
    bool                  allow_first_time_prompts = false;
    bool                  seed_config = true;
    int                   port = 7878;
    std::string           bind_host = "127.0.0.1";  // empty = INADDR_ANY (don't)
};

// Result of preparing the workspace (resolving "tmp" vs. absolute, seeding
// config etc.). Exposed so main.cpp can do the prep before constructing the
// AgenticOptions struct above.
struct WorkspacePrep {
    std::filesystem::path resolved_dir;
    bool                  is_tmp = false;
    std::string           error;          // empty on success
};

WorkspacePrep prepare_agentic_workspace(const std::string& workspace_spec,
                                        const std::filesystem::path& temp_root,
                                        bool seed_config,
                                        bool allow_first_time_prompts);

// Runs the agentic server. Blocks until a `shutdown` request closes it or
// the GUI process exits. Returns the exit code suitable for main().
int run_agentic_server(const AgenticOptions& opts);

} // namespace locus::uia
