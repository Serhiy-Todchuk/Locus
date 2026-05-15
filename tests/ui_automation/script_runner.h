#pragma once

// S5.L -- JSON-driven UIA script runner.
//
// A script is a single file:
//   {
//     "name": "smoke",
//     "description": "human-readable",
//     "setup":   { "workspace": "<absolute path or 'tmp'>" },
//     "steps":   [ {"op":"launch"}, ... ],
//     "cleanup": [ ... ]              // optional
//   }
//
// Each step is `{ "op": "<name>", "args": {...}, "expect"?: {...} }`. The
// list of supported ops lives in the README.

#include "uia_session.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace locus::uia {

struct StepResult {
    bool        ok = true;
    std::string failure;  // populated when ok=false
    std::string detail;   // optional extra info ("found N elements", ...)
};

struct ScriptResult {
    std::string                name;
    bool                       passed = false;
    bool                       skipped = false;
    int                        steps_total    = 0;
    int                        steps_executed = 0;
    std::string                failure_op;
    std::string                failure_reason;
    std::vector<std::string>   step_log;  // one line per executed step
    std::filesystem::path      output_dir;
    long long                  duration_ms = 0;
};

struct RunOptions {
    std::filesystem::path script_path;       // input .json file
    std::filesystem::path locus_gui_path;    // locus_gui.exe
    std::filesystem::path output_root;       // tests/ui_automation/output/
    std::filesystem::path temp_workspace_root; // %TEMP%/locus_ui_tests/<script>/
};

class ScriptRunner {
public:
    using Json = nlohmann::json;

    explicit ScriptRunner(const RunOptions& opts);

    ScriptResult run();

private:
    StepResult run_step(const Json& step);

    // Step ops -- each returns a StepResult.
    StepResult op_launch(const Json& args);
    StepResult op_wait_for_window(const Json& args);
    StepResult op_find(const Json& args);
    StepResult op_click(const Json& args);
    StepResult op_type(const Json& args);
    StepResult op_press_key(const Json& args);
    StepResult op_select_tab(const Json& args);
    StepResult op_get_text(const Json& args);
    StepResult op_assert_text_contains(const Json& args);
    StepResult op_assert_visible(const Json& args);
    StepResult op_screenshot(const Json& args);
    StepResult op_sleep(const Json& args);
    StepResult op_quit(const Json& args);
    StepResult op_dump_tree(const Json& args);
    StepResult op_assert_file_exists(const Json& args);

    Element find_named(const std::string& automation_id, int timeout_ms);
    Element resolve_target(const Json& args, int timeout_ms);

    // Per-step bookkeeping for screenshot filenames.
    int step_index_ = 0;

    RunOptions      opts_;
    UiaSession      uia_;
    Json            script_;
    std::filesystem::path output_dir_;
    std::string     workspace_path_used_;  // resolved at runtime
    // setup.allow_first_time_prompts -- when true, skip the tmp-workspace
    // `.locus/config.json` pre-seed AND the auto-injection of the
    // `--no-first-time-prompts` launch flag, so the capabilities first-open
    // modal can fire for the capabilities_first_open.json script.
    bool            allow_first_time_prompts_ = false;
};

// Convenience: load a script from disk. Throws std::runtime_error on parse
// failure with a precise message; callers translate that into a fail report.
nlohmann::json load_script(const std::filesystem::path& path);

// Write a JUnit-style XML report next to the per-script output dir so CI
// consumers can pick it up later. We write one <testcase> per step plus one
// outer <testsuite>.
bool write_junit_xml(const ScriptResult& result,
                     const std::filesystem::path& xml_path);

} // namespace locus::uia
