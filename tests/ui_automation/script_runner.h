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
// Each step is `{ "op": "<name>", "args": {...} }`. The list of supported ops
// lives in the README; the actual dispatch table is implemented in
// op_dispatcher.{h,cpp}, which S5.Z task 9 extracted so AgenticServer could
// share it.

#include "op_dispatcher.h"
#include "uia_session.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace locus::uia {

struct ScriptResult {
    std::string                name;
    bool                       passed = false;
    bool                       skipped = false;
    int                        steps_total    = 0;
    int                        steps_executed = 0;
    std::string                failure_op;
    std::string                failure_reason;
    std::vector<std::string>   step_log;
    std::filesystem::path      output_dir;
    long long                  duration_ms = 0;
};

struct RunOptions {
    std::filesystem::path script_path;
    std::filesystem::path locus_gui_path;
    std::filesystem::path output_root;
    std::filesystem::path temp_workspace_root;
};

class ScriptRunner {
public:
    using Json = nlohmann::json;

    explicit ScriptRunner(const RunOptions& opts);

    ScriptResult run();

private:
    StepResult run_step(const Json& step);

    RunOptions      opts_;
    UiaSession      uia_;
    std::unique_ptr<OpDispatcher> dispatcher_;
    Json            script_;
    std::filesystem::path output_dir_;
    std::string     workspace_path_used_;
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
