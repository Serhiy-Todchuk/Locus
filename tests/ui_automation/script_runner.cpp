#include "script_runner.h"

#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

namespace locus::uia {

namespace {

std::string short_random_id()
{
    std::mt19937_64 rng((unsigned long long)std::chrono::steady_clock::now()
                            .time_since_epoch().count());
    std::ostringstream os;
    os << std::hex << (rng() & 0xFFFFFFu);
    return os.str();
}

std::string get_string(const ScriptRunner::Json& j, const std::string& key,
                       const std::string& default_value = {})
{
    if (j.is_object() && j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return default_value;
}

} // namespace

nlohmann::json load_script(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open " + path.string());
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& ex) {
        throw std::runtime_error("JSON parse error in " + path.string() + ": " + ex.what());
    }
    if (!j.is_object())
        throw std::runtime_error("script root must be an object");
    return j;
}

ScriptRunner::ScriptRunner(const RunOptions& opts) : opts_(opts) {}

ScriptResult ScriptRunner::run()
{
    auto t_start = std::chrono::steady_clock::now();
    ScriptResult result;
    result.name = opts_.script_path.stem().string();

    try {
        script_ = load_script(opts_.script_path);
    } catch (const std::exception& ex) {
        result.failure_op     = "load_script";
        result.failure_reason = ex.what();
        result.duration_ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        return result;
    }

    // Resolve workspace path. "tmp" means a fresh dir under temp_workspace_root.
    std::filesystem::path workspace_dir;
    bool is_tmp = false;
    if (script_.contains("setup") && script_["setup"].is_object()) {
        const auto& setup = script_["setup"];
        std::string ws = get_string(setup, "workspace", "tmp");
        if (ws == "tmp" || ws.empty()) {
            workspace_dir = opts_.temp_workspace_root / (result.name + "_" + short_random_id());
            std::error_code ec;
            std::filesystem::create_directories(workspace_dir, ec);
            is_tmp = true;
        } else {
            workspace_dir = ws;
        }

        if (setup.contains("allow_first_time_prompts") &&
            setup["allow_first_time_prompts"].is_boolean())
        {
            allow_first_time_prompts_ = setup["allow_first_time_prompts"].get<bool>();
        }

        if (setup.contains("env") && setup["env"].is_object()) {
            for (auto it = setup["env"].begin(); it != setup["env"].end(); ++it) {
                if (!it.value().is_string()) continue;
                std::string val = it.value().get<std::string>();
                size_t p = val.find("{workspace}");
                while (p != std::string::npos) {
                    val.replace(p, 11, workspace_dir.string());
                    p = val.find("{workspace}", p + workspace_dir.string().size());
                }
                ::SetEnvironmentVariableA(it.key().c_str(), val.c_str());
            }
        }
    } else {
        workspace_dir = opts_.temp_workspace_root / (result.name + "_" + short_random_id());
        std::error_code ec;
        std::filesystem::create_directories(workspace_dir, ec);
        is_tmp = true;
    }
    workspace_path_used_ = workspace_dir.string();

    if (is_tmp && !allow_first_time_prompts_) {
        try {
            auto cfg_path = workspace_dir / ".locus" / "config.json";
            std::error_code ec;
            std::filesystem::create_directories(cfg_path.parent_path(), ec);
            Json cfg;
            cfg["index"]["semantic_search"]["enabled"] = false;
            cfg["tool_approvals"] = {
                {"write_file",     "auto"},
                {"edit_file",      "auto"},
                {"delete_file",    "auto"},
                {"run_command",    "auto"},
                {"run_command_bg", "auto"}
            };
            cfg["sessions"]["auto_cleanup_enabled"] = false;
            cfg["sessions"]["restore_last"]        = false;
            if (script_.contains("setup") && script_["setup"].is_object()) {
                const auto& setup = script_["setup"];
                if (setup.contains("sessions_config") &&
                    setup["sessions_config"].is_object())
                {
                    for (auto it = setup["sessions_config"].begin();
                              it != setup["sessions_config"].end(); ++it) {
                        cfg["sessions"][it.key()] = it.value();
                    }
                }
            }
            if (script_.contains("setup") && script_["setup"].is_object()) {
                const auto& setup = script_["setup"];
                if (setup.contains("tool_approvals_override") &&
                    setup["tool_approvals_override"].is_object())
                {
                    for (auto it = setup["tool_approvals_override"].begin();
                              it != setup["tool_approvals_override"].end(); ++it) {
                        if (it.value().is_string())
                            cfg["tool_approvals"][it.key()] = it.value().get<std::string>();
                    }
                }
            }
            if (script_.contains("setup") && script_["setup"].is_object()) {
                const auto& setup = script_["setup"];
                if (setup.contains("capabilities_override") &&
                    setup["capabilities_override"].is_object())
                {
                    for (auto it = setup["capabilities_override"].begin();
                              it != setup["capabilities_override"].end(); ++it) {
                        if (it.value().is_boolean())
                            cfg["capabilities"][it.key()] = it.value().get<bool>();
                    }
                }
            }
            std::ofstream f(cfg_path);
            f << cfg.dump(2) << '\n';
        } catch (const std::exception& ex) {
            (void)ex;
        }

        try {
            if (script_.contains("setup") && script_["setup"].is_object() &&
                script_["setup"].contains("sessions_seed") &&
                script_["setup"]["sessions_seed"].is_array())
            {
                auto sessions_dir = workspace_dir / ".locus" / "sessions";
                std::error_code ec2;
                std::filesystem::create_directories(sessions_dir, ec2);
                long long now_s =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                int counter = 0;
                for (const auto& entry : script_["setup"]["sessions_seed"]) {
                    if (!entry.is_object()) continue;
                    std::string id = entry.value("id",
                        std::string("seed_") + std::to_string(++counter));
                    std::string title = entry.value("title", id);
                    long long when = entry.value("last_opened_at", now_s);
                    if (when < 0) when = now_s + when;

                    // Optional `messages` array -- when supplied, persisted
                    // verbatim so UIA tests can stage a saved session with
                    // reasoning_content + tool_calls + tool-result rows and
                    // verify the restore path renders them correctly.
                    Json messages = Json::array();
                    if (entry.contains("messages") && entry["messages"].is_array())
                        messages = entry["messages"];

                    // Optional top-level extras (agent_mode + plan +
                    // plan_awaiting_decision) -- used by the save/restore
                    // UIA test. Merge into the session JSON last so we can
                    // selectively override defaults.
                    Json extras = entry.value("extras", Json::object());

                    Json j;
                    j["id"]                = id;
                    j["message_count"]     = static_cast<int>(messages.size());
                    j["estimated_tokens"]  = entry.value("estimated_tokens", 100);
                    j["messages"]          = std::move(messages);
                    j["first_user_message"] = entry.value("first_user_message",
                                                          title);
                    j["title"]             = title;
                    j["created_at"]        = when;
                    j["last_opened_at"]    = when;
                    if (extras.is_object()) {
                        for (auto it = extras.begin(); it != extras.end(); ++it)
                            j[it.key()] = it.value();
                    }
                    std::ofstream sf(sessions_dir / (id + ".json"));
                    sf << j.dump(2) << '\n';
                }
            }
        } catch (const std::exception& ex) {
            (void)ex;
        }

        // Optional: seed `.locus/ui_state.json` so the workspace's
        // restore_last hook auto-opens specific sessions as tabs. Shape
        // matches `WorkspaceUiState`: { "open_tabs": [{ session_id, title,
        // active }] }. Used by save/restore tests to stage a known set of
        // tabs without driving them through the menu first.
        try {
            if (script_.contains("setup") && script_["setup"].is_object() &&
                script_["setup"].contains("ui_state") &&
                script_["setup"]["ui_state"].is_object())
            {
                auto locus_dir = workspace_dir / ".locus";
                std::error_code ec3;
                std::filesystem::create_directories(locus_dir, ec3);
                std::ofstream usf(locus_dir / "ui_state.json");
                usf << script_["setup"]["ui_state"].dump(2) << '\n';
            }
        } catch (const std::exception& ex) {
            (void)ex;
        }
    }

    output_dir_ = opts_.output_root / result.name;
    std::error_code ec;
    std::filesystem::create_directories(output_dir_, ec);
    result.output_dir = output_dir_;

    std::ofstream log(output_dir_ / "run.log");
    log << "script:     " << opts_.script_path.string() << "\n";
    log << "workspace:  " << workspace_dir.string()     << "\n";
    log << "locus_gui:  " << opts_.locus_gui_path.string() << "\n\n";

    if (!uia_.ready()) {
        result.failure_op     = "init";
        result.failure_reason = "UIA COM init failed: " + uia_.last_error();
        log << "FAIL: " << result.failure_reason << "\n";
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        return result;
    }

    // Compose the dispatcher now that workspace + output_dir are known.
    dispatcher_ = std::make_unique<OpDispatcher>(
        uia_, opts_.locus_gui_path, output_dir_, workspace_path_used_,
        allow_first_time_prompts_);

    if (!script_.contains("steps") || !script_["steps"].is_array()) {
        result.failure_op     = "script";
        result.failure_reason = "missing 'steps' array";
        log << "FAIL: " << result.failure_reason << "\n";
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        return result;
    }

    auto& steps = script_["steps"];
    result.steps_total = (int)steps.size();

    bool failed = false;
    for (size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps[i];

        std::string op = get_string(step, "op");
        log << "[" << (i + 1) << "/" << steps.size() << "] op=" << op;

        StepResult sr = run_step(step);

        ++result.steps_executed;
        std::ostringstream line;
        line << "step " << (i + 1) << " op=" << op
             << " result=" << (sr.ok ? "ok" : "FAIL");
        if (!sr.detail.empty()) line << " detail=" << sr.detail;
        if (!sr.ok)             line << " reason=" << sr.failure;
        result.step_log.push_back(line.str());

        if (!sr.ok) {
            log << "  FAIL: " << sr.failure << "\n";
            result.failure_op     = op;
            result.failure_reason = sr.failure;
            failed = true;
            break;
        } else if (!sr.detail.empty()) {
            log << "  ok (" << sr.detail << ")\n";
        } else {
            log << "  ok\n";
        }
    }

    if (script_.contains("cleanup") && script_["cleanup"].is_array()) {
        log << "\n-- cleanup --\n";
        for (const auto& step : script_["cleanup"]) {
            std::string op = get_string(step, "op");
            log << "cleanup op=" << op << "\n";
            run_step(step);
        }
    }

    uia_.close();

    result.passed       = !failed;
    result.duration_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();
    log << "\nresult: " << (result.passed ? "PASS" : "FAIL")
        << " (" << result.duration_ms << " ms)\n";
    return result;
}

StepResult ScriptRunner::run_step(const Json& step)
{
    if (!step.is_object())
        return StepResult{ false, "step must be an object", {}, nullptr };
    std::string op = get_string(step, "op");
    Json args = step.contains("args") ? step["args"] : Json::object();
    if (!dispatcher_)
        return StepResult{ false, "dispatcher not initialised", {}, nullptr };
    return dispatcher_->dispatch(op, args);
}

bool write_junit_xml(const ScriptResult& result, const std::filesystem::path& xml_path)
{
    std::ofstream out(xml_path);
    if (!out) return false;

    int failures = result.passed ? 0 : 1;
    int total    = result.steps_total;
    double duration_s = result.duration_ms / 1000.0;

    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<testsuite name=\"" << result.name
        << "\" tests=\"" << total
        << "\" failures=\"" << failures
        << "\" time=\"" << duration_s
        << "\">\n";

    for (size_t i = 0; i < result.step_log.size(); ++i) {
        out << "  <testcase classname=\"" << result.name
            << "\" name=\"step_" << (i + 1) << "\" time=\"0\">\n";
        if (!result.passed && (i + 1) == result.step_log.size()) {
            out << "    <failure message=\""
                << result.failure_reason
                << "\">" << result.step_log[i] << "</failure>\n";
        }
        out << "  </testcase>\n";
    }

    out << "</testsuite>\n";
    return true;
}

} // namespace locus::uia
