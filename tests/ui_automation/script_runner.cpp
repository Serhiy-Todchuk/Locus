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

int get_int(const ScriptRunner::Json& j, const std::string& key, int default_value)
{
    if (j.is_object() && j.contains(key) && j[key].is_number_integer())
        return j[key].get<int>();
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
    if (script_.contains("setup") && script_["setup"].is_object()) {
        std::string ws = get_string(script_["setup"], "workspace", "tmp");
        if (ws == "tmp" || ws.empty()) {
            workspace_dir = opts_.temp_workspace_root / (result.name + "_" + short_random_id());
            std::error_code ec;
            std::filesystem::create_directories(workspace_dir, ec);
        } else {
            workspace_dir = ws;
        }
    } else {
        workspace_dir = opts_.temp_workspace_root / (result.name + "_" + short_random_id());
        std::error_code ec;
        std::filesystem::create_directories(workspace_dir, ec);
    }
    workspace_path_used_ = workspace_dir.string();

    // Set up output directory.
    output_dir_ = opts_.output_root / result.name;
    std::error_code ec;
    std::filesystem::create_directories(output_dir_, ec);
    result.output_dir = output_dir_;

    // Open the per-script log.
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

    // Steps.
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
        step_index_ = (int)(i + 1);

        std::string op = get_string(step, "op");
        log << "[" << step_index_ << "/" << steps.size() << "] op=" << op;

        StepResult sr = run_step(step);

        ++result.steps_executed;
        std::ostringstream line;
        line << "step " << step_index_ << " op=" << op
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

    // Cleanup steps (best-effort -- failures here don't change pass/fail).
    if (script_.contains("cleanup") && script_["cleanup"].is_array()) {
        log << "\n-- cleanup --\n";
        for (const auto& step : script_["cleanup"]) {
            std::string op = get_string(step, "op");
            log << "cleanup op=" << op << "\n";
            run_step(step);  // ignore failures
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

// ---------------------------------------------------------------------------
// Step dispatch
// ---------------------------------------------------------------------------

StepResult ScriptRunner::run_step(const Json& step)
{
    if (!step.is_object())
        return StepResult{ false, "step must be an object", {} };
    std::string op = get_string(step, "op");
    Json args = step.contains("args") ? step["args"] : Json::object();

    if (op == "launch")                  return op_launch(args);
    if (op == "wait_for_window")         return op_wait_for_window(args);
    if (op == "find")                    return op_find(args);
    if (op == "click")                   return op_click(args);
    if (op == "type")                    return op_type(args);
    if (op == "press_key")               return op_press_key(args);
    if (op == "select_tab")              return op_select_tab(args);
    if (op == "get_text")                return op_get_text(args);
    if (op == "assert_text_contains")    return op_assert_text_contains(args);
    if (op == "assert_visible")          return op_assert_visible(args);
    if (op == "screenshot")              return op_screenshot(args);
    if (op == "sleep")                   return op_sleep(args);
    if (op == "quit")                    return op_quit(args);
    if (op == "dump_tree")               return op_dump_tree(args);

    return StepResult{ false, "unknown op '" + op + "'", {} };
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Element ScriptRunner::find_named(const std::string& automation_id, int timeout_ms)
{
    FindQuery q;
    q.automation_id = automation_id;
    q.timeout_ms    = timeout_ms;
    return uia_.find(q);
}

// Map a string control-type spec ("edit", "document", "button", "tab",
// "tabitem", "text", "pane", "checkbox", "tree", "treeitem") to a UIA
// control-type id. Unknown -> 0 (caller treats as "no filter").
static int parse_control_type(const std::string& s)
{
    std::string n; n.reserve(s.size());
    for (char c : s) n.push_back((char)std::tolower((unsigned char)c));
    if (n == "edit")     return UIA_EditControlTypeId;
    if (n == "document") return UIA_DocumentControlTypeId;
    if (n == "button")   return UIA_ButtonControlTypeId;
    if (n == "tab")      return UIA_TabControlTypeId;
    if (n == "tabitem")  return UIA_TabItemControlTypeId;
    if (n == "text")     return UIA_TextControlTypeId;
    if (n == "pane")     return UIA_PaneControlTypeId;
    if (n == "checkbox") return UIA_CheckBoxControlTypeId;
    if (n == "tree")     return UIA_TreeControlTypeId;
    if (n == "treeitem") return UIA_TreeItemControlTypeId;
    if (n == "window")   return UIA_WindowControlTypeId;
    if (n == "menu")     return UIA_MenuControlTypeId;
    if (n == "menuitem") return UIA_MenuItemControlTypeId;
    return 0;
}

// Resolve the target element for a find / click / type step. Honors:
//   automation_id      direct lookup (Name OR AutomationId, see uia_session)
//   name               accessible-name match
//   parent_aid + control_type
//                      find the parent by automation_id, then search inside
//                      for the first element matching control_type (and
//                      optionally name).
//
// Returns an invalid Element on failure with last_error set on uia_.
Element ScriptRunner::resolve_target(const Json& args, int timeout_ms)
{
    std::string aid        = get_string(args, "automation_id");
    std::string name       = get_string(args, "name");
    std::string parent_aid = get_string(args, "parent_aid");
    std::string ct_str     = get_string(args, "control_type");
    int ct                 = parse_control_type(ct_str);

    if (!parent_aid.empty()) {
        Element parent = find_named(parent_aid, timeout_ms);
        if (!parent.valid()) return Element{};
        FindQuery q;
        q.name        = name;          // optional
        if (ct) q.control_type = ct;
        q.timeout_ms  = timeout_ms;
        return uia_.find(q, &parent);
    }

    FindQuery q;
    q.automation_id = aid;
    q.name          = name;
    if (ct) q.control_type = ct;
    q.timeout_ms    = timeout_ms;
    return uia_.find(q);
}

// ---------------------------------------------------------------------------
// Step ops
// ---------------------------------------------------------------------------

StepResult ScriptRunner::op_launch(const Json& args)
{
    // Always pass --no-first-time-prompts so a fresh tmp workspace doesn't
    // block on the semantic-search modal. Scripts may add more args via
    // `extra_args` -- order matters; we put the flag first so a user-supplied
    // override (vanishingly unlikely) could still take effect downstream.
    std::vector<std::string> extra{ "--no-first-time-prompts" };
    if (args.contains("extra_args") && args["extra_args"].is_array()) {
        for (auto& v : args["extra_args"])
            if (v.is_string()) extra.push_back(v.get<std::string>());
    }
    if (!uia_.launch(opts_.locus_gui_path, workspace_path_used_, extra))
        return { false, uia_.last_error(), {} };
    return { true, {}, "pid=" + std::to_string(uia_.process_id()) };
}

StepResult ScriptRunner::op_wait_for_window(const Json& args)
{
    std::string prefix = get_string(args, "title_prefix", "Locus");
    int timeout = get_int(args, "timeout_ms", 15000);
    Element el = uia_.wait_for_window(prefix, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {} };
    return { true, {}, "hwnd=" + std::to_string((uintptr_t)uia_.window()) };
}

StepResult ScriptRunner::op_find(const Json& args)
{
    int timeout = get_int(args, "timeout_ms", 5000);
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {} };
    return { true, {}, "name=" + el.name() };
}

StepResult ScriptRunner::op_click(const Json& args)
{
    int timeout = get_int(args, "timeout_ms", 5000);
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {} };
    if (!uia_.click(el)) return { false, uia_.last_error(), {} };
    return { true, {}, "clicked" };
}

StepResult ScriptRunner::op_type(const Json& args)
{
    std::string text = get_string(args, "text");
    int timeout      = get_int(args, "timeout_ms", 5000);
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {} };
    if (!uia_.type_text(el, text)) return { false, uia_.last_error(), {} };
    return { true, {}, "typed " + std::to_string(text.size()) + " chars" };
}

StepResult ScriptRunner::op_press_key(const Json& args)
{
    std::string key  = get_string(args, "key");
    std::string mods = get_string(args, "modifiers");
    if (key.empty()) return { false, "press_key: requires 'key'", {} };

    // If the caller scoped a target (automation_id / parent_aid /
    // control_type), route the key directly to that HWND via PostMessage --
    // reliable regardless of foreground focus. Otherwise fall back to
    // SendInput against the OS focus.
    bool has_target = !get_string(args, "automation_id").empty()
                    || !get_string(args, "name").empty()
                    || !get_string(args, "parent_aid").empty();
    if (has_target) {
        int timeout = get_int(args, "timeout_ms", 5000);
        Element el = resolve_target(args, timeout);
        if (!el.valid()) return { false, uia_.last_error(), {} };
        if (!uia_.press_key_to(el, key, mods))
            return { false, uia_.last_error(), {} };
        return { true, {}, "posted " + (mods.empty() ? key : mods + "+" + key) };
    }
    if (!uia_.press_key(key, mods)) return { false, uia_.last_error(), {} };
    return { true, {}, "pressed " + (mods.empty() ? key : mods + "+" + key) };
}

StepResult ScriptRunner::op_select_tab(const Json& args)
{
    // wxNotebook on Windows wraps SysTabControl32; UIA exposes each tab as
    // a TabItem under a Tab parent. The TabItems have empty Name (native
    // tab control doesn't propagate labels to MSAA), so name-based lookup
    // doesn't work for `wxNotebook` -- callers must instead specify either
    //   `index`             0-based child index under the matched Tab parent
    //   `parent_aid`        the AutomationId / Name of the containing Tab
    //   `name`              fallback for wxAuiNotebook / custom tab strips
    // that DO surface labels.
    std::string name        = get_string(args, "name");
    std::string parent_aid  = get_string(args, "parent_aid");
    int  timeout            = get_int(args, "timeout_ms", 5000);
    bool has_index          = args.is_object() && args.contains("index")
                              && args["index"].is_number_integer();
    int  index              = has_index ? args["index"].get<int>() : -1;

    // Index path -- needs the parent Tab control to enumerate children.
    if (has_index) {
        if (parent_aid.empty())
            return { false, "select_tab: 'index' requires 'parent_aid'", {} };
        FindQuery pq;
        pq.automation_id = parent_aid;
        pq.control_type  = UIA_TabControlTypeId;
        pq.timeout_ms    = timeout;
        Element parent = uia_.find(pq);
        if (!parent.valid()) return { false, uia_.last_error(), {} };

        // Walk Tab children, count TabItems, click the requested index.
        // Implemented via UIA TreeWalker on TabItem control type.
        // FindAll under TreeScope_Children would be cleaner, but constructing
        // the SAFEARRAY-of-conditions there is more code than this loop.
        IUIAutomationCondition* cond = nullptr;
        VARIANT v; VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = UIA_TabItemControlTypeId;
        IUIAutomation* automation_raw = nullptr;
        // Re-acquire automation via CUIAutomation -- the UiaSession owns one
        // but we don't expose it; cheaper to just borrow Element's raw().
        CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                         __uuidof(IUIAutomation),
                         reinterpret_cast<void**>(&automation_raw));
        if (!automation_raw) return { false, "select_tab: CUIAutomation unavailable", {} };
        automation_raw->CreatePropertyCondition(
            UIA_ControlTypePropertyId, v, &cond);
        VariantClear(&v);
        IUIAutomationElementArray* arr = nullptr;
        HRESULT hr = parent.raw()->FindAll(TreeScope_Children, cond, &arr);
        if (cond) cond->Release();
        if (FAILED(hr) || !arr) {
            automation_raw->Release();
            return { false, "select_tab: failed to enumerate TabItems", {} };
        }
        int len = 0;
        arr->get_Length(&len);
        if (index < 0 || index >= len) {
            arr->Release();
            automation_raw->Release();
            return { false,
                "select_tab: index " + std::to_string(index) +
                " out of range (" + std::to_string(len) + " tabs)",
                {} };
        }
        IUIAutomationElement* tab_raw = nullptr;
        arr->GetElement(index, &tab_raw);
        arr->Release();
        automation_raw->Release();
        if (!tab_raw) return { false, "select_tab: GetElement returned null", {} };
        Element tab(tab_raw);
        if (!uia_.click(tab)) return { false, uia_.last_error(), {} };
        return { true, {}, "selected tab #" + std::to_string(index) };
    }

    // Name path (wxAuiNotebook + any control that exposes Name).
    if (name.empty())
        return { false, "select_tab: requires either 'name' or 'index'+'parent_aid'", {} };
    FindQuery q;
    q.name         = name;
    q.control_type = UIA_TabItemControlTypeId;
    q.timeout_ms   = timeout;
    Element el = uia_.find(q);
    if (!el.valid()) return op_click(args);  // last resort: generic click
    if (!uia_.click(el)) return { false, uia_.last_error(), {} };
    return { true, {}, "selected tab " + name };
}

StepResult ScriptRunner::op_get_text(const Json& args)
{
    int timeout       = get_int(args, "timeout_ms", 5000);
    bool walk_subtree = args.contains("walk_subtree")
                        && args["walk_subtree"].is_boolean()
                        && args["walk_subtree"].get<bool>();
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {} };
    std::string text = walk_subtree ? uia_.read_all_text(el) : uia_.read_text(el);
    if (text.size() > 200) text = text.substr(0, 200) + "...";
    return { true, {}, "text=" + text };
}

StepResult ScriptRunner::op_assert_text_contains(const Json& args)
{
    std::string needle = get_string(args, "substring");
    int         timeout = get_int(args, "timeout_ms", 5000);
    bool walk_subtree = args.contains("walk_subtree")
                        && args["walk_subtree"].is_boolean()
                        && args["walk_subtree"].get<bool>();
    if (needle.empty())
        return { false, "assert_text_contains: requires 'substring'", {} };

    bool ok = uia_.wait_for([&] {
        Element el = resolve_target(args, 500);
        if (!el.valid()) return false;
        std::string text = walk_subtree ? uia_.read_all_text(el) : uia_.read_text(el);
        return text.find(needle) != std::string::npos;
    }, timeout);

    if (!ok) {
        return { false,
                 "assert_text_contains: '" + needle + "' not seen in " +
                 std::to_string(timeout) + " ms",
                 {} };
    }
    return { true, {}, "found '" + needle + "'" };
}

StepResult ScriptRunner::op_assert_visible(const Json& args)
{
    int timeout = get_int(args, "timeout_ms", 5000);
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {} };
    if (el.is_offscreen()) return { false, "element is offscreen", {} };
    return { true, {}, "visible" };
}

StepResult ScriptRunner::op_screenshot(const Json& args)
{
    std::string name = get_string(args, "name");
    if (name.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "step_%02d.png", step_index_);
        name = buf;
    } else if (name.find('.') == std::string::npos) {
        name += ".png";
    }
    auto path = output_dir_ / name;
    if (!uia_.screenshot(path)) return { false, uia_.last_error(), {} };
    return { true, {}, "saved " + path.string() };
}

StepResult ScriptRunner::op_sleep(const Json& args)
{
    int ms = get_int(args, "ms", 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return { true, {}, std::to_string(ms) + " ms" };
}

StepResult ScriptRunner::op_quit(const Json& args)
{
    int wait = get_int(args, "wait_ms", 3000);
    uia_.close(wait);
    return { true, {}, {} };
}

StepResult ScriptRunner::op_dump_tree(const Json& args)
{
    int depth = get_int(args, "depth", 8);
    std::string name = get_string(args, "name", "tree.txt");
    std::string dump = uia_.dump_tree(nullptr, depth);
    auto path = output_dir_ / name;
    std::ofstream out(path);
    if (!out) return { false, "dump_tree: cannot write " + path.string(), {} };
    out << dump;
    return { true, {}, "wrote " + path.string() };
}

// ---------------------------------------------------------------------------
// JUnit XML output
// ---------------------------------------------------------------------------

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

    // One testcase per executed step.
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
