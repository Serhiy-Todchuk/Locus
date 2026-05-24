#pragma once

// S5.Z task 9 -- Op dispatcher shared between scripted runs and agentic mode.
//
// The S5.L script runner originally owned all op_* handlers as private member
// functions. Agentic mode (TCP/JSON, driven by an LLM "QA agent") needs the
// same dispatch surface plus a handful of richer ops (read chat WebView,
// submit chat message, list workspace, read locus.log, etc.). This file
// extracts the dispatch table so both ScriptRunner and AgenticServer compose
// the same primitive.
//
// The shared state is small: UiaSession reference, locus_gui path (for
// op_launch), output dir (for screenshots / tree dumps), workspace path (for
// file ops), a step counter (for default screenshot names), and the
// allow_first_time_prompts flag (for op_launch's extra-args injection).

#include "uia_session.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace locus::uia {

struct StepResult {
    bool           ok      = true;
    std::string    failure;        // populated when ok = false
    std::string    detail;         // free-text payload ("found N elements", ...)
    nlohmann::json data = nullptr; // optional structured payload (agentic mode)
};

class OpDispatcher {
public:
    using Json = nlohmann::json;

    OpDispatcher(UiaSession& uia,
                 std::filesystem::path locus_gui_path,
                 std::filesystem::path output_dir,
                 std::string           workspace_path,
                 bool                  allow_first_time_prompts,
                 bool                  agentic_mode = false);

    // Look up `op` in the dispatch table, hand `args` to the matching handler.
    // Unknown ops yield ok=false. Each dispatch increments step_index_ so
    // screenshot default names stay monotonic across the run.
    StepResult dispatch(const std::string& op, const Json& args);

    UiaSession&        uia() { return uia_; }
    int                step_index() const { return step_index_; }
    void               set_step_index(int v) { step_index_ = v; }
    const std::string& workspace_path() const { return workspace_path_; }
    const std::filesystem::path& output_dir()  const { return output_dir_; }
    const std::filesystem::path& locus_gui_path() const { return locus_gui_path_; }

private:
    // Existing ops (S5.L) -- one to one with the script step ops.
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
    StepResult op_assert_file_contains(const Json& args);

    // Agentic-mode extensions (S5.Z task 9).
    StepResult op_submit_chat(const Json& args);
    StepResult op_read_chat(const Json& args);
    StepResult op_wait_for_text_stable(const Json& args);
    StepResult op_wait_for_log_contains(const Json& args);
    StepResult op_list_workspace(const Json& args);
    StepResult op_read_workspace_file(const Json& args);
    StepResult op_write_workspace_file(const Json& args);
    StepResult op_delete_workspace_file(const Json& args);
    StepResult op_read_locus_log(const Json& args);
    StepResult op_get_chat_status(const Json& args);
    StepResult op_list_named_widgets(const Json& args);

    Element find_named(const std::string& automation_id, int timeout_ms);
    Element resolve_target(const Json& args, int timeout_ms);

    // Workspace-relative path resolver that refuses to escape the workspace
    // root (catches `..` traversal, absolute paths outside the root, etc.).
    // Returns an empty path + sets `error` on rejection.
    std::filesystem::path resolve_workspace_path(const std::string& rel,
                                                 std::string& error) const;

    UiaSession&             uia_;
    std::filesystem::path   locus_gui_path_;
    std::filesystem::path   output_dir_;
    std::string             workspace_path_;
    int                     step_index_ = 0;
    bool                    allow_first_time_prompts_ = false;
    // When true (agentic-server only), op_launch injects --agentic-mute-noise
    // so the trace stream in .locus/locus.log isn't drowned in SQL / file-
    // watcher chatter. Scripted-runner stays false (it captures stdout/stderr
    // for parallel runs, not the workspace log).
    bool                    agentic_mode_ = false;
};

} // namespace locus::uia
