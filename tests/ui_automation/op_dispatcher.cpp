// S5.Z task 9 -- implementation of the shared dispatch table.

#include "op_dispatcher.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace locus::uia {

namespace {

std::string get_string(const nlohmann::json& j, const std::string& key,
                       const std::string& default_value = {})
{
    if (j.is_object() && j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return default_value;
}

int get_int(const nlohmann::json& j, const std::string& key, int default_value)
{
    if (j.is_object() && j.contains(key) && j[key].is_number_integer())
        return j[key].get<int>();
    return default_value;
}

bool get_bool(const nlohmann::json& j, const std::string& key, bool default_value)
{
    if (j.is_object() && j.contains(key) && j[key].is_boolean())
        return j[key].get<bool>();
    return default_value;
}

// Map a string control-type spec ("edit", "document", "button", "tab",
// "tabitem", "text", "pane", "checkbox", "tree", "treeitem") to a UIA
// control-type id. Unknown -> 0 (caller treats as "no filter").
int parse_control_type(const std::string& s)
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

// Read up to `max_bytes` bytes from `path`. Returns ok=false if the file
// can't be opened. Sets `truncated=true` when the file was larger than
// the cap.
bool read_file_bytes(const fs::path& path, size_t max_bytes,
                     std::string& out, bool& truncated, size_t& file_size)
{
    std::error_code ec;
    file_size = (size_t)fs::file_size(path, ec);
    if (ec) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    truncated = file_size > max_bytes;
    size_t to_read = std::min(file_size, max_bytes);
    out.resize(to_read);
    if (to_read > 0) in.read(out.data(), (std::streamsize)to_read);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// OpDispatcher
// ---------------------------------------------------------------------------

OpDispatcher::OpDispatcher(UiaSession& uia,
                           fs::path locus_gui_path,
                           fs::path output_dir,
                           std::string workspace_path,
                           bool allow_first_time_prompts)
  : uia_(uia),
    locus_gui_path_(std::move(locus_gui_path)),
    output_dir_(std::move(output_dir)),
    workspace_path_(std::move(workspace_path)),
    allow_first_time_prompts_(allow_first_time_prompts)
{}

StepResult OpDispatcher::dispatch(const std::string& op, const Json& args)
{
    ++step_index_;

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
    if (op == "assert_file_exists")      return op_assert_file_exists(args);
    if (op == "assert_file_contains")    return op_assert_file_contains(args);

    if (op == "submit_chat")             return op_submit_chat(args);
    if (op == "read_chat")               return op_read_chat(args);
    if (op == "wait_for_text_stable")    return op_wait_for_text_stable(args);
    if (op == "wait_for_log_contains")   return op_wait_for_log_contains(args);
    if (op == "list_workspace")          return op_list_workspace(args);
    if (op == "read_workspace_file")     return op_read_workspace_file(args);
    if (op == "write_workspace_file")    return op_write_workspace_file(args);
    if (op == "delete_workspace_file")   return op_delete_workspace_file(args);
    if (op == "read_locus_log")          return op_read_locus_log(args);
    if (op == "get_chat_status")         return op_get_chat_status(args);
    if (op == "list_named_widgets")      return op_list_named_widgets(args);

    return StepResult{ false, "unknown op '" + op + "'", {}, nullptr };
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Element OpDispatcher::find_named(const std::string& automation_id, int timeout_ms)
{
    FindQuery q;
    q.automation_id = automation_id;
    q.timeout_ms    = timeout_ms;
    return uia_.find(q);
}

Element OpDispatcher::resolve_target(const Json& args, int timeout_ms)
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
        q.name        = name;
        if (ct) q.control_type = ct;
        q.timeout_ms  = timeout_ms;
        bool deep = get_bool(args, "deep", false);
        q.deep = deep;
        Element first = uia_.find(q, &parent);
        if (first.valid() || deep) return first;
        // Fallback: caller didn't ask for deep, but no direct child matched.
        q.deep = true;
        return uia_.find(q, &parent);
    }

    FindQuery q;
    q.automation_id = aid;
    q.name          = name;
    if (ct) q.control_type = ct;
    q.timeout_ms    = timeout_ms;
    return uia_.find(q);
}

fs::path OpDispatcher::resolve_workspace_path(const std::string& rel,
                                              std::string& error) const
{
    if (rel.empty()) { error = "empty path"; return {}; }
    fs::path root = fs::path(workspace_path_).lexically_normal();
    fs::path candidate = (root / rel).lexically_normal();

    // Component-wise containment check (case-insensitive on Windows).
    auto root_str = root.string();
    auto cand_str = candidate.string();
    auto eq_icase = [](char a, char b) {
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    };
    if (cand_str.size() < root_str.size()
        || !std::equal(root_str.begin(), root_str.end(), cand_str.begin(), eq_icase))
    {
        error = "path '" + rel + "' resolves outside workspace";
        return {};
    }
    // Make sure the next char is a separator (so /ws-evil isn't accepted as
    // a child of /ws).
    if (cand_str.size() > root_str.size()) {
        char next = cand_str[root_str.size()];
        if (next != '/' && next != '\\') {
            error = "path '" + rel + "' resolves outside workspace";
            return {};
        }
    }
    return candidate;
}

// ---------------------------------------------------------------------------
// Step ops -- existing (S5.L)
// ---------------------------------------------------------------------------

StepResult OpDispatcher::op_launch(const Json& args)
{
    std::vector<std::string> extra;
    if (!allow_first_time_prompts_)
        extra.push_back("--no-first-time-prompts");
    if (args.contains("extra_args") && args["extra_args"].is_array()) {
        for (auto& v : args["extra_args"])
            if (v.is_string()) extra.push_back(v.get<std::string>());
    }
    if (!uia_.launch(locus_gui_path_, workspace_path_, extra))
        return { false, uia_.last_error(), {}, nullptr };
    return { true, {}, "pid=" + std::to_string(uia_.process_id()), nullptr };
}

StepResult OpDispatcher::op_wait_for_window(const Json& args)
{
    std::string prefix = get_string(args, "title_prefix", "Locus");
    int timeout = get_int(args, "timeout_ms", 15000);
    Element el = uia_.wait_for_window(prefix, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {}, nullptr };
    return { true, {}, "hwnd=" + std::to_string((uintptr_t)uia_.window()), nullptr };
}

StepResult OpDispatcher::op_find(const Json& args)
{
    int timeout = get_int(args, "timeout_ms", 5000);
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {}, nullptr };
    return { true, {}, "name=" + el.name(), nullptr };
}

StepResult OpDispatcher::op_click(const Json& args)
{
    int timeout = get_int(args, "timeout_ms", 5000);
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {}, nullptr };
    if (!uia_.click(el)) return { false, uia_.last_error(), {}, nullptr };
    return { true, {}, "clicked", nullptr };
}

StepResult OpDispatcher::op_type(const Json& args)
{
    std::string text = get_string(args, "text");
    int timeout      = get_int(args, "timeout_ms", 5000);
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {}, nullptr };
    if (!uia_.type_text(el, text)) return { false, uia_.last_error(), {}, nullptr };
    return { true, {}, "typed " + std::to_string(text.size()) + " chars", nullptr };
}

StepResult OpDispatcher::op_press_key(const Json& args)
{
    std::string key  = get_string(args, "key");
    std::string mods = get_string(args, "modifiers");
    if (key.empty()) return { false, "press_key: requires 'key'", {}, nullptr };

    bool has_target = !get_string(args, "automation_id").empty()
                    || !get_string(args, "name").empty()
                    || !get_string(args, "parent_aid").empty();
    if (has_target) {
        int timeout = get_int(args, "timeout_ms", 5000);
        Element el = resolve_target(args, timeout);
        if (!el.valid()) return { false, uia_.last_error(), {}, nullptr };
        if (!uia_.press_key_to(el, key, mods))
            return { false, uia_.last_error(), {}, nullptr };
        return { true, {}, "posted " + (mods.empty() ? key : mods + "+" + key), nullptr };
    }
    if (!uia_.press_key(key, mods)) return { false, uia_.last_error(), {}, nullptr };
    return { true, {}, "pressed " + (mods.empty() ? key : mods + "+" + key), nullptr };
}

StepResult OpDispatcher::op_select_tab(const Json& args)
{
    std::string name        = get_string(args, "name");
    std::string parent_aid  = get_string(args, "parent_aid");
    int  timeout            = get_int(args, "timeout_ms", 5000);
    bool has_index          = args.is_object() && args.contains("index")
                              && args["index"].is_number_integer();
    int  index              = has_index ? args["index"].get<int>() : -1;

    if (has_index) {
        if (parent_aid.empty())
            return { false, "select_tab: 'index' requires 'parent_aid'", {}, nullptr };
        FindQuery pq;
        pq.automation_id = parent_aid;
        pq.control_type  = UIA_TabControlTypeId;
        pq.timeout_ms    = timeout;
        Element parent = uia_.find(pq);
        if (!parent.valid()) return { false, uia_.last_error(), {}, nullptr };

        IUIAutomationCondition* cond = nullptr;
        VARIANT v; VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = UIA_TabItemControlTypeId;
        IUIAutomation* automation_raw = nullptr;
        CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                         __uuidof(IUIAutomation),
                         reinterpret_cast<void**>(&automation_raw));
        if (!automation_raw) return { false, "select_tab: CUIAutomation unavailable", {}, nullptr };
        automation_raw->CreatePropertyCondition(
            UIA_ControlTypePropertyId, v, &cond);
        VariantClear(&v);
        IUIAutomationElementArray* arr = nullptr;
        HRESULT hr = parent.raw()->FindAll(TreeScope_Children, cond, &arr);
        if (cond) cond->Release();
        if (FAILED(hr) || !arr) {
            automation_raw->Release();
            return { false, "select_tab: failed to enumerate TabItems", {}, nullptr };
        }
        int len = 0;
        arr->get_Length(&len);
        if (index < 0 || index >= len) {
            arr->Release();
            automation_raw->Release();
            return { false,
                "select_tab: index " + std::to_string(index) +
                " out of range (" + std::to_string(len) + " tabs)",
                {}, nullptr };
        }
        IUIAutomationElement* tab_raw = nullptr;
        arr->GetElement(index, &tab_raw);
        arr->Release();
        automation_raw->Release();
        if (!tab_raw) return { false, "select_tab: GetElement returned null", {}, nullptr };
        Element tab(tab_raw);
        if (!uia_.click(tab)) return { false, uia_.last_error(), {}, nullptr };
        return { true, {}, "selected tab #" + std::to_string(index), nullptr };
    }

    if (name.empty())
        return { false, "select_tab: requires either 'name' or 'index'+'parent_aid'", {}, nullptr };
    FindQuery q;
    q.name         = name;
    q.control_type = UIA_TabItemControlTypeId;
    q.timeout_ms   = timeout;
    Element el = uia_.find(q);
    if (!el.valid()) return op_click(args);
    if (!uia_.click(el)) return { false, uia_.last_error(), {}, nullptr };
    return { true, {}, "selected tab " + name, nullptr };
}

StepResult OpDispatcher::op_get_text(const Json& args)
{
    int timeout       = get_int(args, "timeout_ms", 5000);
    bool walk_subtree = get_bool(args, "walk_subtree", false);
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {}, nullptr };
    std::string text = walk_subtree ? uia_.read_all_text(el) : uia_.read_text(el);
    Json data;
    data["text"]   = text;
    data["length"] = (int)text.size();
    std::string preview = text;
    if (preview.size() > 200) preview = preview.substr(0, 200) + "...";
    return { true, {}, "text=" + preview, std::move(data) };
}

StepResult OpDispatcher::op_assert_text_contains(const Json& args)
{
    std::string needle = get_string(args, "substring");
    int         timeout = get_int(args, "timeout_ms", 5000);
    bool walk_subtree = get_bool(args, "walk_subtree", false);
    if (needle.empty())
        return { false, "assert_text_contains: requires 'substring'", {}, nullptr };

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
                 {}, nullptr };
    }
    return { true, {}, "found '" + needle + "'", nullptr };
}

StepResult OpDispatcher::op_assert_visible(const Json& args)
{
    int timeout = get_int(args, "timeout_ms", 5000);
    Element el = resolve_target(args, timeout);
    if (!el.valid()) return { false, uia_.last_error(), {}, nullptr };
    if (el.is_offscreen()) return { false, "element is offscreen", {}, nullptr };
    return { true, {}, "visible", nullptr };
}

StepResult OpDispatcher::op_screenshot(const Json& args)
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
    if (!uia_.screenshot(path)) return { false, uia_.last_error(), {}, nullptr };
    Json data;
    data["path"] = path.string();
    return { true, {}, "saved " + path.string(), std::move(data) };
}

StepResult OpDispatcher::op_sleep(const Json& args)
{
    int ms = get_int(args, "ms", 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return { true, {}, std::to_string(ms) + " ms", nullptr };
}

StepResult OpDispatcher::op_quit(const Json& args)
{
    int wait = get_int(args, "wait_ms", 3000);
    uia_.close(wait);
    return { true, {}, {}, nullptr };
}

StepResult OpDispatcher::op_dump_tree(const Json& args)
{
    int depth = get_int(args, "depth", 8);
    std::string name = get_string(args, "name", "tree.txt");
    std::string dump = uia_.dump_tree(nullptr, depth);
    auto path = output_dir_ / name;
    std::ofstream out(path);
    if (!out) return { false, "dump_tree: cannot write " + path.string(), {}, nullptr };
    out << dump;
    Json data;
    data["path"] = path.string();
    data["bytes"] = (int)dump.size();
    return { true, {}, "wrote " + path.string(), std::move(data) };
}

StepResult OpDispatcher::op_assert_file_exists(const Json& args)
{
    std::string rel = get_string(args, "path");
    int timeout    = get_int(args, "timeout_ms", 5000);
    bool any_match = args.contains("any_of") && args["any_of"].is_array();

    if (rel.empty() && !any_match)
        return { false, "assert_file_exists: requires 'path' or 'any_of'", {}, nullptr };

    std::vector<std::string> candidates;
    if (!rel.empty()) candidates.push_back(rel);
    if (any_match) {
        for (auto& v : args["any_of"])
            if (v.is_string()) candidates.push_back(v.get<std::string>());
    }

    fs::path root = workspace_path_;
    bool ok = uia_.wait_for([&] {
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::exists(root / c, ec)) return true;
        }
        return false;
    }, timeout);

    if (!ok) {
        std::string list;
        for (auto& c : candidates) {
            if (!list.empty()) list += ", ";
            list += c;
        }
        return { false,
                 "assert_file_exists: none of [" + list + "] exist in "
                 + root.string() + " after " + std::to_string(timeout) + " ms",
                 {}, nullptr };
    }
    return { true, {}, "file appeared", nullptr };
}

StepResult OpDispatcher::op_assert_file_contains(const Json& args)
{
    std::string rel       = get_string(args, "path");
    std::string substring = get_string(args, "substring");
    int         timeout   = get_int(args, "timeout_ms", 5000);

    if (rel.empty())
        return { false, "assert_file_contains: requires 'path'", {}, nullptr };
    if (substring.empty())
        return { false, "assert_file_contains: requires 'substring'", {}, nullptr };

    fs::path full = fs::path(workspace_path_) / rel;

    bool ok = uia_.wait_for([&] {
        std::error_code ec;
        if (!fs::exists(full, ec)) return false;
        std::ifstream in(full, std::ios::binary);
        if (!in) return false;
        std::string contents((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        return contents.find(substring) != std::string::npos;
    }, timeout);

    if (!ok) {
        return { false,
                 "assert_file_contains: '" + substring +
                 "' not found in " + full.string() +
                 " after " + std::to_string(timeout) + " ms",
                 {}, nullptr };
    }
    return { true, {}, "substring present", nullptr };
}

// ---------------------------------------------------------------------------
// Step ops -- agentic extensions
// ---------------------------------------------------------------------------

StepResult OpDispatcher::op_submit_chat(const Json& args)
{
    std::string text = get_string(args, "text");
    int timeout = get_int(args, "timeout_ms", 5000);
    if (text.empty())
        return { false, "submit_chat: requires 'text'", {}, nullptr };

    // The chat input is a wxTE_RICH2 textctrl; the wxAccessible shim doesn't
    // override the native RichEdit's IAccessible, so by automation_id the
    // name lookup fails. Address by control type under locus.chat.panel.
    FindQuery q;
    q.automation_id = "locus.chat.input";
    q.timeout_ms    = timeout;
    Element el = uia_.find(q);
    if (!el.valid()) {
        // Fallback: edit/document under the chat panel.
        Element parent = find_named("locus.chat.panel", timeout);
        if (!parent.valid()) return { false, "submit_chat: chat panel not found", {}, nullptr };
        FindQuery fq;
        fq.control_type = UIA_DocumentControlTypeId;
        fq.timeout_ms   = timeout;
        fq.deep         = false;
        el = uia_.find(fq, &parent);
        if (!el.valid()) {
            FindQuery eq;
            eq.control_type = UIA_EditControlTypeId;
            eq.timeout_ms   = timeout;
            eq.deep         = true;
            el = uia_.find(eq, &parent);
        }
        if (!el.valid()) return { false, "submit_chat: chat input not found", {}, nullptr };
    }
    if (!uia_.type_text(el, text)) return { false, uia_.last_error(), {}, nullptr };
    if (!uia_.press_key_to(el, "ENTER", {}))
        return { false, uia_.last_error(), {}, nullptr };
    return { true, {}, "submitted " + std::to_string(text.size()) + " chars", nullptr };
}

StepResult OpDispatcher::op_read_chat(const Json& args)
{
    int  timeout = get_int(args, "timeout_ms", 5000);
    int  max_chars = get_int(args, "max_chars", 20000);
    bool from_webview = get_bool(args, "from_webview", true);
    Element el;
    if (from_webview) {
        el = find_named("locus.chat.webview", timeout);
        if (!el.valid()) el = find_named("locus.chat.panel", timeout);
    } else {
        el = find_named("locus.chat.panel", timeout);
    }
    if (!el.valid()) return { false, uia_.last_error(), {}, nullptr };
    std::string text = uia_.read_all_text(el);
    bool truncated = (int)text.size() > max_chars;
    if (truncated) text = text.substr(0, max_chars);
    Json data;
    data["text"]      = text;
    data["truncated"] = truncated;
    data["length"]    = (int)text.size();
    return { true, {}, "read " + std::to_string(text.size()) + " chars" +
                       (truncated ? " (truncated)" : ""), std::move(data) };
}

StepResult OpDispatcher::op_wait_for_text_stable(const Json& args)
{
    int stable_ms  = get_int(args, "stable_ms", 3000);
    int timeout_ms = get_int(args, "timeout_ms", 120000);
    int poll_ms    = get_int(args, "poll_ms", 500);
    bool walk      = get_bool(args, "walk_subtree", true);
    int min_chars  = get_int(args, "min_chars", 0);

    auto start = std::chrono::steady_clock::now();
    std::string last_text;
    auto last_change = start;
    int polls = 0;
    while (true) {
        Element el = resolve_target(args, 500);
        std::string text = el.valid()
            ? (walk ? uia_.read_all_text(el) : uia_.read_text(el))
            : std::string{};
        ++polls;
        if (text != last_text) {
            last_text = text;
            last_change = std::chrono::steady_clock::now();
        }
        auto now = std::chrono::steady_clock::now();
        long long since_change_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_change).count();
        long long since_start_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

        if ((int)last_text.size() >= min_chars && since_change_ms >= stable_ms) {
            Json data;
            data["polls"]       = polls;
            data["length"]      = (int)last_text.size();
            data["elapsed_ms"]  = (int)since_start_ms;
            return { true, {}, "stable after " + std::to_string(since_start_ms) + " ms",
                     std::move(data) };
        }
        if (since_start_ms >= timeout_ms) {
            Json data;
            data["polls"]       = polls;
            data["length"]      = (int)last_text.size();
            data["elapsed_ms"]  = (int)since_start_ms;
            return { false,
                     "wait_for_text_stable: text still changing after " +
                     std::to_string(since_start_ms) + " ms (last len " +
                     std::to_string(last_text.size()) + ")",
                     {}, std::move(data) };
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
}

StepResult OpDispatcher::op_wait_for_log_contains(const Json& args)
{
    std::string needle = get_string(args, "substring");
    int timeout_ms = get_int(args, "timeout_ms", 60000);
    int poll_ms    = get_int(args, "poll_ms", 500);
    long long since_byte = 0;
    if (args.is_object() && args.contains("since_byte")
        && args["since_byte"].is_number_integer())
    {
        since_byte = args["since_byte"].get<long long>();
    }
    if (needle.empty())
        return { false, "wait_for_log_contains: requires 'substring'", {}, nullptr };

    fs::path log_path = fs::path(workspace_path_) / ".locus" / "locus.log";

    auto start = std::chrono::steady_clock::now();
    while (true) {
        std::error_code ec;
        auto size = fs::exists(log_path, ec) ? (long long)fs::file_size(log_path, ec) : 0LL;
        if (size > since_byte) {
            std::ifstream in(log_path, std::ios::binary);
            if (in) {
                in.seekg((std::streamoff)since_byte);
                std::string chunk((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
                auto pos = chunk.find(needle);
                if (pos != std::string::npos) {
                    Json data;
                    data["matched_at_byte"] = since_byte + (long long)pos;
                    data["log_size"]        = size;
                    return { true, {},
                             "matched at byte " +
                             std::to_string(since_byte + (long long)pos),
                             std::move(data) };
                }
            }
        }
        auto now = std::chrono::steady_clock::now();
        long long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (elapsed >= timeout_ms) {
            Json data;
            data["log_size"] = size;
            return { false,
                     "wait_for_log_contains: '" + needle + "' not seen in " +
                     std::to_string(elapsed) + " ms",
                     {}, std::move(data) };
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
}

StepResult OpDispatcher::op_list_workspace(const Json& args)
{
    std::string rel_path = get_string(args, "path", "");
    int max_depth        = get_int(args, "depth", 3);
    int max_entries      = get_int(args, "max_entries", 500);
    bool include_hidden  = get_bool(args, "include_hidden", false);

    fs::path start;
    if (rel_path.empty()) {
        start = workspace_path_;
    } else {
        std::string err;
        start = resolve_workspace_path(rel_path, err);
        if (start.empty()) return { false, "list_workspace: " + err, {}, nullptr };
    }
    std::error_code ec;
    if (!fs::exists(start, ec))
        return { false, "list_workspace: path does not exist", {}, nullptr };

    // Default-exclude set (mirrors the file_tree panel + indexer exclusions)
    // so the agentic client doesn't drown in build artefacts.
    auto is_excluded = [&](const fs::path& p) -> bool {
        std::string name = p.filename().string();
        if (!include_hidden && !name.empty() && name[0] == '.') {
            if (name == ".locus") return false;  // surface .locus/ contents
            return true;
        }
        if (name == "build" || name == "build_release" || name == "build_debug"
            || name == "node_modules" || name == "target" || name == "dist"
            || name == "out" || name == ".git") return true;
        return false;
    };

    Json entries = Json::array();
    fs::path root = workspace_path_;
    int  count = 0;
    bool truncated = false;

    // Manual recursive walk so we can prune excluded directories before
    // descending into them.
    std::vector<std::pair<fs::path, int>> stack;
    stack.emplace_back(start, 0);
    while (!stack.empty()) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        if (depth > max_depth) continue;
        std::error_code dec;
        for (auto& entry : fs::directory_iterator(dir, dec)) {
            if (is_excluded(entry.path())) continue;
            if (count >= max_entries) { truncated = true; break; }
            ++count;
            Json item;
            std::error_code rec;
            fs::path rel_p = fs::relative(entry.path(), root, rec);
            item["path"]   = rel_p.generic_string();
            item["is_dir"] = entry.is_directory(dec);
            if (entry.is_regular_file(dec)) {
                std::error_code sec;
                item["size"] = (long long)fs::file_size(entry.path(), sec);
            }
            entries.push_back(std::move(item));
            if (entry.is_directory(dec) && depth + 1 <= max_depth)
                stack.emplace_back(entry.path(), depth + 1);
        }
        if (truncated) break;
    }

    Json data;
    data["entries"]   = std::move(entries);
    data["count"]     = count;
    data["truncated"] = truncated;
    return { true, {},
             std::to_string(count) + (truncated ? "+ entries (truncated)" : " entries"),
             std::move(data) };
}

StepResult OpDispatcher::op_read_workspace_file(const Json& args)
{
    std::string rel = get_string(args, "path");
    int max_bytes   = get_int(args, "max_bytes", 200000);
    if (rel.empty()) return { false, "read_workspace_file: requires 'path'", {}, nullptr };
    std::string err;
    fs::path full = resolve_workspace_path(rel, err);
    if (full.empty()) return { false, "read_workspace_file: " + err, {}, nullptr };

    std::error_code ec;
    if (!fs::exists(full, ec))
        return { false, "read_workspace_file: not found", {}, nullptr };
    if (fs::is_directory(full, ec))
        return { false, "read_workspace_file: is a directory", {}, nullptr };

    std::string text;
    bool truncated = false;
    size_t file_size = 0;
    if (!read_file_bytes(full, (size_t)max_bytes, text, truncated, file_size))
        return { false, "read_workspace_file: cannot read", {}, nullptr };

    Json data;
    data["text"]      = text;
    data["truncated"] = truncated;
    data["size"]      = (long long)file_size;
    data["length"]    = (int)text.size();
    return { true, {},
             "read " + std::to_string(text.size()) + " of " +
             std::to_string(file_size) + " bytes" +
             (truncated ? " (truncated)" : ""),
             std::move(data) };
}

StepResult OpDispatcher::op_write_workspace_file(const Json& args)
{
    std::string rel  = get_string(args, "path");
    std::string text = get_string(args, "content");
    if (rel.empty()) return { false, "write_workspace_file: requires 'path'", {}, nullptr };
    std::string err;
    fs::path full = resolve_workspace_path(rel, err);
    if (full.empty()) return { false, "write_workspace_file: " + err, {}, nullptr };

    std::error_code ec;
    fs::create_directories(full.parent_path(), ec);
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    if (!out) return { false, "write_workspace_file: cannot open for write", {}, nullptr };
    out.write(text.data(), (std::streamsize)text.size());
    out.close();
    Json data;
    data["path"]  = full.string();
    data["bytes"] = (int)text.size();
    return { true, {}, "wrote " + std::to_string(text.size()) + " bytes",
             std::move(data) };
}

StepResult OpDispatcher::op_delete_workspace_file(const Json& args)
{
    std::string rel = get_string(args, "path");
    if (rel.empty()) return { false, "delete_workspace_file: requires 'path'", {}, nullptr };
    std::string err;
    fs::path full = resolve_workspace_path(rel, err);
    if (full.empty()) return { false, "delete_workspace_file: " + err, {}, nullptr };
    std::error_code ec;
    if (!fs::exists(full, ec))
        return { true, {}, "did not exist", nullptr };
    bool ok = fs::remove(full, ec);
    if (!ok) return { false, "delete_workspace_file: " + ec.message(), {}, nullptr };
    return { true, {}, "deleted", nullptr };
}

StepResult OpDispatcher::op_read_locus_log(const Json& args)
{
    int tail_lines = get_int(args, "tail_lines", 200);
    long long since_byte = 0;
    if (args.is_object() && args.contains("since_byte")
        && args["since_byte"].is_number_integer())
    {
        since_byte = args["since_byte"].get<long long>();
    }

    fs::path log_path = fs::path(workspace_path_) / ".locus" / "locus.log";
    std::error_code ec;
    if (!fs::exists(log_path, ec)) {
        Json data;
        data["lines"]   = Json::array();
        data["size"]    = 0;
        data["present"] = false;
        return { true, {}, "log not yet created", std::move(data) };
    }
    long long size = (long long)fs::file_size(log_path, ec);

    std::ifstream in(log_path, std::ios::binary);
    if (!in) return { false, "read_locus_log: cannot open", {}, nullptr };
    if (since_byte > 0 && since_byte <= size) {
        in.seekg((std::streamoff)since_byte);
    }
    std::string chunk((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    // Split into lines (LF / CRLF tolerant).
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= chunk.size(); ++i) {
        if (i == chunk.size() || chunk[i] == '\n') {
            std::string line = chunk.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() || i != chunk.size()) lines.push_back(std::move(line));
            start = i + 1;
        }
    }
    if (since_byte == 0 && (int)lines.size() > tail_lines) {
        lines.erase(lines.begin(), lines.end() - tail_lines);
    }

    Json data;
    data["lines"]   = lines;
    data["size"]    = size;
    data["present"] = true;
    return { true, {}, std::to_string(lines.size()) + " lines",
             std::move(data) };
}

StepResult OpDispatcher::op_get_chat_status(const Json& args)
{
    int timeout = get_int(args, "timeout_ms", 1000);
    auto safe_read = [&](const char* aid) -> std::string {
        Element el = find_named(aid, timeout);
        if (!el.valid()) return {};
        std::string t = uia_.read_text(el);
        if (t.empty()) t = el.name();
        return t;
    };
    Json data;
    data["context_label"]  = safe_read("locus.chat.ctx_label");
    data["plan_chip"]      = safe_read("locus.chat.plan_chip");
    data["commit_chip"]    = safe_read("locus.chat.commit_chip");
    data["compacted_chip"] = safe_read("locus.chat.compacted_chip");
    data["preset_chip"]    = safe_read("locus.chat.preset_chip");
    // Stop button visibility is a proxy for "agent is busy".
    Element stop = find_named("locus.chat.stop_btn", timeout);
    data["stop_btn_visible"] = stop.valid() && !stop.is_offscreen();
    return { true, {}, "status snapshot", std::move(data) };
}

StepResult OpDispatcher::op_list_named_widgets(const Json& args)
{
    int depth = get_int(args, "depth", 12);
    std::string dump = uia_.dump_tree(nullptr, depth);
    // The dump format from uia_session is `<indent>ControlType  Name  AutomationId`.
    // Filter to lines that mention an automation_id starting with "locus.".
    std::vector<std::string> matched;
    size_t start = 0;
    for (size_t i = 0; i <= dump.size(); ++i) {
        if (i == dump.size() || dump[i] == '\n') {
            std::string line = dump.substr(start, i - start);
            if (line.find("locus.") != std::string::npos) {
                matched.push_back(std::move(line));
            }
            start = i + 1;
        }
    }
    Json data;
    data["lines"] = matched;
    data["count"] = (int)matched.size();
    return { true, {}, std::to_string(matched.size()) + " named widgets",
             std::move(data) };
}

} // namespace locus::uia
