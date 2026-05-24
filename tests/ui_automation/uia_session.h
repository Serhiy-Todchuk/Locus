#pragma once

// S5.L -- thin C++ helper around Windows UI Automation (UIA) COM.
//
// Goal: hide the verbose COM ceremony behind a small "find then act" surface
// so script steps in script_runner.cpp read like a recipe rather than 30
// lines of QueryInterface boilerplate per operation.
//
// Threading: UIA is single-threaded apartment by convention; we initialise
// COM with COINIT_APARTMENTTHREADED in UiaSession::UiaSession.
//
// Lifetime: one UiaSession per script run. The session owns one launched
// locus_gui.exe child process; on destruction we send WM_CLOSE then wait
// (timeout) before terminating. Killing in destruction is intentional --
// scripts can crash, hang, or leak, and the harness must not leave wedged
// GUI windows behind for the next script.

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>
#include <uiautomation.h>

namespace locus::uia {

// Identifies one UIA element. Wraps a `Microsoft::WRL`-style raw pointer
// with a manual AddRef/Release so we don't pull in WRL or COMUtils.
// Move-only.
class Element {
public:
    Element() = default;
    explicit Element(IUIAutomationElement* raw);
    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;
    Element(Element&& o) noexcept;
    Element& operator=(Element&& o) noexcept;
    ~Element();

    // True if this Element references a real UI node.
    bool valid() const { return raw_ != nullptr; }
    IUIAutomationElement* raw() const { return raw_; }

    // Convenience accessors. Each property fetch may fail when the element
    // is stale (window closed) -- we return empty strings in that case.
    std::string name() const;
    std::string automation_id() const;
    int         control_type() const;  // UIA_*ControlTypeId values
    bool        is_offscreen() const;
    // The UIA Value pattern's CurrentValue. For wx widgets that go through
    // LocusAccessible::GetValue (currently only wxButton), this returns the
    // visible label -- a stable way to read state without touching the
    // accessible Name (which we already use as the locator). Returns "" when
    // the element doesn't support the Value pattern.
    std::string value() const;

private:
    IUIAutomationElement* raw_ = nullptr;
};

// Query for finding a control. Empty fields are ignored. Multiple fields
// AND-combine.
struct FindQuery {
    std::string                automation_id;   // SetName() value (see ui_names.h)
    std::string                name;            // accessible name (button label, etc.)
    std::optional<int>         control_type;    // UIA_*ControlTypeId; std::nullopt = any
    int                        timeout_ms = 5000;
    bool                       deep = true;     // walk the whole subtree, not just children
};

// Scripted UIA session. Owns the COM `CUIAutomation` singleton, the
// launched child process, and the root element.
class UiaSession {
public:
    UiaSession();
    ~UiaSession();

    UiaSession(const UiaSession&) = delete;
    UiaSession& operator=(const UiaSession&) = delete;

    // True once the COM stack is up. False on init failure.
    bool ready() const { return automation_ != nullptr; }

    // Launch the GUI. Returns true on successful CreateProcessW; the window
    // may not be visible yet -- call wait_for_window() after to gate on it.
    // `workspace_path` is passed as argv[1] so the picker dialog is skipped.
    // Any extra command-line tokens are appended verbatim.
    bool launch(const std::filesystem::path& exe_path,
                const std::filesystem::path& workspace_path,
                const std::vector<std::string>& extra_args = {});

    // Wait until a top-level window owned by the launched process has its
    // title starting with `title_prefix`. Returns the matched Element, or
    // an invalid Element on timeout. Stores the matched HWND for screenshots.
    Element wait_for_window(const std::string& title_prefix, int timeout_ms);

    // Find one element under the given root (or the desktop root if `root`
    // is invalid). Retries until `query.timeout_ms` elapses. Returns an
    // invalid Element on timeout.
    Element find(const FindQuery& query, const Element* root = nullptr);

    // Click the centre of the element using UIA's `InvokePattern` if
    // available, falling back to a synthesised mouse click at the element's
    // bounding-rect centre. Returns false if neither path worked.
    bool click(const Element& el);

    // Send the given UTF-8 text into the focused control. Newlines are
    // sent verbatim. For Enter-as-submit, use press_key("ENTER") after.
    // Returns false on a SendInput failure.
    bool type_text(const Element& el, const std::string& text);

    // Synthesise a single key press (down + up). `key_name` is one of:
    //   ENTER, ESC, TAB, SPACE, BACKSPACE, UP, DOWN, LEFT, RIGHT,
    //   COMMA, F1..F12.
    // `modifiers` may include any of "CTRL", "ALT", "SHIFT" (comma- or
    // plus-separated). Modifiers are pressed before and released after
    // the main key. Unknown keys return false.
    bool press_key(const std::string& key_name,
                   const std::string& modifiers = {});

    // Same as press_key, but PostMessage the key sequence directly to
    // `target`'s HWND. More reliable than SendInput when the script
    // wants the key to land on a specific control regardless of which
    // window currently has foreground focus.
    bool press_key_to(const Element& target,
                      const std::string& key_name,
                      const std::string& modifiers = {});

    // Read the human-visible text from an element. For Text controls this
    // is the value; for Edit controls we use ValuePattern.CurrentValue;
    // for everything else we fall back to the accessible name.
    std::string read_text(const Element& el);

    // Walk all `Text` descendants of `root` and concatenate their values
    // with newlines. Used to scrape the WebView's rendered chat content.
    std::string read_all_text(const Element& root);

    // Wait up to `timeout_ms` for `predicate` to return true. Polls every
    // 100ms (50ms first, exponential back-off capped at 250ms). Returns
    // the final predicate result.
    bool wait_for(std::function<bool()> predicate, int timeout_ms);

    // Capture a screenshot of the launched window to `out_path` (PNG).
    // Uses PrintWindow with PW_RENDERFULLCONTENT so WebView2 content is
    // included. No-op (returns false) if no window has been latched yet.
    bool screenshot(const std::filesystem::path& out_path);

    // Terminate the child if still alive. Called automatically by ~.
    void close(int wait_ms = 3000);

    // Debug: walk the UIA subtree under `root` to depth `max_depth`,
    // returning a multi-line dump of "ControlType  Name  AutomationId".
    // When `root` is invalid, dumps every visible top-level window of our
    // process. Diagnostic only; called by the dump_tree script op.
    std::string dump_tree(const Element* root, int max_depth = 8);

    // Last error message produced by any of the calls above. Useful in
    // failure paths -- the script runner reports this in the FAIL line.
    const std::string& last_error() const { return last_error_; }

    // Process / window handles for callers that need direct access.
    HWND        window() const { return target_hwnd_; }
    DWORD       process_id() const { return process_id_; }

    // True when no process has been launched yet, OR the launched process is
    // still running. False ONLY when launch() succeeded but the child has
    // since exited. Callers like find() / wait_for_window() use this to fail
    // fast with a "gui process exited" diagnostic instead of letting the
    // long UIA timeout elapse and reporting a misleading
    // "find: timeout waiting for element" message.
    bool is_launched_process_alive() const;

private:
    bool ensure_automation();
    Element find_in_root(IUIAutomationElement* root, const FindQuery& query);
    void set_error(const std::string& msg);

    IUIAutomation*        automation_   = nullptr;
    IUIAutomationElement* desktop_root_ = nullptr;
    HANDLE                process_handle_ = nullptr;
    DWORD                 process_id_   = 0;
    HWND                  target_hwnd_  = nullptr;
    std::string           last_error_;
    bool                  com_initialised_ = false;
};

// PNG writer used by `screenshot`. Standalone so the test suite can call
// it on a captured HBITMAP directly. Returns false on file-write failure.
bool save_hbitmap_as_png(HBITMAP bitmap, const std::filesystem::path& out_path);

// Convert a UTF-8 std::string to UTF-16 std::wstring (Win32 API takes
// wide strings everywhere). Empty input yields empty output.
std::wstring utf8_to_wide(const std::string& s);
std::string  wide_to_utf8(const std::wstring& w);

} // namespace locus::uia
