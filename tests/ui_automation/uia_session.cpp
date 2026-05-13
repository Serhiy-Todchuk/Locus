// Defined before windows.h so the win32 macros `min`/`max` never expand --
// otherwise `std::min(...)` becomes garbled.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "uia_session.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

#include <sstream>

#include <objbase.h>
#include <oleauto.h>
#include <wingdi.h>

// WIC bitmap encoder for PNG output.
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace locus::uia {

// ---------------------------------------------------------------------------
// String conversion helpers
// ---------------------------------------------------------------------------

std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

static std::string bstr_to_utf8(BSTR b)
{
    if (!b) return {};
    UINT n = SysStringLen(b);
    return wide_to_utf8(std::wstring(b, b + n));
}

// ---------------------------------------------------------------------------
// Element
// ---------------------------------------------------------------------------

Element::Element(IUIAutomationElement* raw) : raw_(raw)
{
    // Caller transfers ownership -- they already hold an AddRef.
}

Element::Element(Element&& o) noexcept : raw_(o.raw_) { o.raw_ = nullptr; }

Element& Element::operator=(Element&& o) noexcept
{
    if (this != &o) {
        if (raw_) raw_->Release();
        raw_ = o.raw_;
        o.raw_ = nullptr;
    }
    return *this;
}

Element::~Element()
{
    if (raw_) raw_->Release();
}

std::string Element::name() const
{
    if (!raw_) return {};
    BSTR b = nullptr;
    if (FAILED(raw_->get_CurrentName(&b)) || !b) return {};
    std::string out = bstr_to_utf8(b);
    SysFreeString(b);
    return out;
}

std::string Element::automation_id() const
{
    if (!raw_) return {};
    BSTR b = nullptr;
    if (FAILED(raw_->get_CurrentAutomationId(&b)) || !b) return {};
    std::string out = bstr_to_utf8(b);
    SysFreeString(b);
    return out;
}

int Element::control_type() const
{
    if (!raw_) return 0;
    CONTROLTYPEID t = 0;
    if (FAILED(raw_->get_CurrentControlType(&t))) return 0;
    return (int)t;
}

bool Element::is_offscreen() const
{
    if (!raw_) return true;
    BOOL b = FALSE;
    if (FAILED(raw_->get_CurrentIsOffscreen(&b))) return true;
    return b == TRUE;
}

// ---------------------------------------------------------------------------
// UiaSession
// ---------------------------------------------------------------------------

UiaSession::UiaSession()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
        com_initialised_ = SUCCEEDED(hr);
        ensure_automation();
    } else {
        set_error("CoInitializeEx failed");
    }
}

UiaSession::~UiaSession()
{
    close();
    if (desktop_root_) { desktop_root_->Release(); desktop_root_ = nullptr; }
    if (automation_)   { automation_->Release();   automation_   = nullptr; }
    if (com_initialised_) CoUninitialize();
}

bool UiaSession::ensure_automation()
{
    if (automation_) return true;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  __uuidof(IUIAutomation),
                                  reinterpret_cast<void**>(&automation_));
    if (FAILED(hr) || !automation_) {
        set_error("CoCreateInstance(CUIAutomation) failed");
        automation_ = nullptr;
        return false;
    }
    hr = automation_->GetRootElement(&desktop_root_);
    if (FAILED(hr) || !desktop_root_) {
        set_error("IUIAutomation::GetRootElement failed");
        return false;
    }
    return true;
}

void UiaSession::set_error(const std::string& msg)
{
    last_error_ = msg;
}

bool UiaSession::launch(const std::filesystem::path& exe_path,
                        const std::filesystem::path& workspace_path,
                        const std::vector<std::string>& extra_args)
{
    if (!ready()) return false;
    if (process_handle_) {
        set_error("launch() called twice on the same UiaSession");
        return false;
    }

    // Build the command line. CreateProcessW's lpCommandLine must be writable.
    std::wstring cmd = L"\"" + exe_path.wstring() + L"\" \""
                     + workspace_path.wstring() + L"\"";
    for (const auto& a : extra_args) {
        cmd += L" ";
        cmd += utf8_to_wide(a);
    }
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // Run with the parent's working dir; locus_gui doesn't care, but
    // explicit makes the launch reproducible from different CWDs.
    BOOL ok = CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr,
                             FALSE, 0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        DWORD err = GetLastError();
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "CreateProcessW failed with GetLastError()=%lu", err);
        set_error(buf);
        return false;
    }

    if (pi.hThread) CloseHandle(pi.hThread);
    process_handle_ = pi.hProcess;
    process_id_     = pi.dwProcessId;
    return true;
}

Element UiaSession::wait_for_window(const std::string& title_prefix, int timeout_ms)
{
    if (!ready()) return Element{};

    std::wstring prefix_w = utf8_to_wide(title_prefix);

    // EnumWindows-style search over visible top-level HWNDs belonging to
    // our process. UIA itself has a slower equivalent; the Win32 walk is
    // both faster and gives us the HWND we need for screenshots.
    struct Ctx {
        DWORD       pid;
        std::wstring prefix;
        HWND        match = nullptr;
    } ctx{ process_id_, prefix_w };

    auto enum_cb = [](HWND hwnd, LPARAM lp) -> BOOL {
        Ctx& c = *reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c.pid) return TRUE;
        if (!IsWindowVisible(hwnd)) return TRUE;
        // Top-level only: skip windows owned by another of our windows.
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        wchar_t title[512] = { 0 };
        GetWindowTextW(hwnd, title, 511);
        std::wstring t = title;
        if (c.prefix.empty() || t.rfind(c.prefix, 0) == 0) {
            c.match = hwnd;
            return FALSE;  // stop
        }
        return TRUE;
    };

    auto start = std::chrono::steady_clock::now();
    int delay  = 50;
    while (true) {
        ctx.match = nullptr;
        EnumWindows(enum_cb, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.match) {
            target_hwnd_ = ctx.match;
            IUIAutomationElement* el = nullptr;
            if (SUCCEEDED(automation_->ElementFromHandle(ctx.match, &el)) && el) {
                return Element{el};
            }
            // Fall through to retry if UIA didn't bind to the HWND yet.
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            set_error("wait_for_window: timeout waiting for window titled '" +
                      title_prefix + "'");
            return Element{};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        delay = std::min(delay * 2, 250);
    }
}

Element UiaSession::find_in_root(IUIAutomationElement* root, const FindQuery& q)
{
    if (!automation_ || !root) return Element{};

    // Build a string-property condition (BSTR variant + CreatePropertyCondition).
    auto make_string_cond = [&](PROPERTYID pid, const std::string& val) -> IUIAutomationCondition* {
        if (val.empty()) return nullptr;
        std::wstring w = utf8_to_wide(val);
        VARIANT v;
        VariantInit(&v);
        v.vt      = VT_BSTR;
        v.bstrVal = SysAllocStringLen(w.c_str(), (UINT)w.size());
        IUIAutomationCondition* c = nullptr;
        automation_->CreatePropertyCondition(pid, v, &c);
        VariantClear(&v);
        return c;
    };

    // The big wrinkle: wxWidgets maps wxWindow::SetName() to the
    // accessibility Name (UIA_NamePropertyId), NOT to AutomationId. So when
    // a script passes "automation_id=foo", we look for either an exact
    // AutomationId match OR a Name match -- the user-visible identifier
    // stays "automation_id" in scripts (it's an opaque internal id), but
    // the lookup transparently bridges both UIA properties.
    std::vector<IUIAutomationCondition*> conds;

    if (!q.automation_id.empty()) {
        IUIAutomationCondition* by_id   = make_string_cond(UIA_AutomationIdPropertyId, q.automation_id);
        IUIAutomationCondition* by_name = make_string_cond(UIA_NamePropertyId,         q.automation_id);
        if (by_id && by_name) {
            SAFEARRAY* sa = SafeArrayCreateVector(VT_UNKNOWN, 0, 2);
            LONG i0 = 0, i1 = 1;
            SafeArrayPutElement(sa, &i0, by_id);
            SafeArrayPutElement(sa, &i1, by_name);
            IUIAutomationCondition* or_cond = nullptr;
            automation_->CreateOrConditionFromArray(sa, &or_cond);
            SafeArrayDestroy(sa);
            by_id->Release(); by_name->Release();
            if (or_cond) conds.push_back(or_cond);
        } else if (by_id) {
            conds.push_back(by_id);
        } else if (by_name) {
            conds.push_back(by_name);
        }
    }

    if (auto* c = make_string_cond(UIA_NamePropertyId, q.name)) conds.push_back(c);

    if (q.control_type) {
        VARIANT v;
        VariantInit(&v);
        v.vt    = VT_I4;
        v.lVal  = *q.control_type;
        IUIAutomationCondition* c = nullptr;
        if (SUCCEEDED(automation_->CreatePropertyCondition(
                UIA_ControlTypePropertyId, v, &c)) && c) {
            conds.push_back(c);
        }
        VariantClear(&v);
    }

    IUIAutomationCondition* combined = nullptr;
    if (conds.empty()) {
        // FindAll on TrueCondition would scan the world -- require at
        // least one filter so the script doesn't pin the harness.
        set_error("find: query must specify automation_id, name, or control_type");
        return Element{};
    } else if (conds.size() == 1) {
        combined = conds[0];
        combined->AddRef();
    } else {
        SAFEARRAY* sa = SafeArrayCreateVector(VT_UNKNOWN, 0, (ULONG)conds.size());
        for (LONG i = 0; i < (LONG)conds.size(); ++i) {
            SafeArrayPutElement(sa, &i, conds[i]);
        }
        automation_->CreateAndConditionFromArray(sa, &combined);
        SafeArrayDestroy(sa);
    }
    for (auto* c : conds) c->Release();

    IUIAutomationElement* found = nullptr;
    TreeScope scope = q.deep ? TreeScope_Subtree : TreeScope_Children;
    HRESULT hr = root->FindFirst(scope, combined, &found);
    if (combined) combined->Release();

    if (FAILED(hr) || !found) return Element{};
    return Element{found};
}

// Enumerate visible top-level windows belonging to our process. The Settings
// dialog and the main frame are separate top-level HWNDs -- searching the
// main frame's UIA subtree won't reach the dialog's tab items.
static std::vector<HWND> enum_process_top_level(DWORD pid)
{
    struct Ctx { DWORD pid; std::vector<HWND> out; } ctx{ pid, {} };
    auto cb = [](HWND hwnd, LPARAM lp) -> BOOL {
        Ctx& c = *reinterpret_cast<Ctx*>(lp);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == c.pid && IsWindowVisible(hwnd))
            c.out.push_back(hwnd);
        return TRUE;
    };
    EnumWindows(cb, reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

Element UiaSession::find(const FindQuery& query, const Element* root)
{
    if (!ready()) return Element{};

    // When the caller supplies an explicit root, search inside that subtree.
    // Otherwise, iterate every visible top-level HWND of our process so the
    // search reaches the main frame plus any open dialogs (Settings,
    // Compaction, etc.). Cheaper than scanning the desktop subtree and
    // immune to name collisions with other apps.
    auto start = std::chrono::steady_clock::now();
    int delay  = 50;
    Element result;

    while (true) {
        if (root && root->valid()) {
            result = find_in_root(root->raw(), query);
        } else {
            for (HWND hwnd : enum_process_top_level(process_id_)) {
                IUIAutomationElement* el = nullptr;
                if (FAILED(automation_->ElementFromHandle(hwnd, &el)) || !el)
                    continue;
                result = find_in_root(el, query);
                el->Release();
                if (result.valid()) break;
            }
        }

        if (result.valid()) break;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= query.timeout_ms) {
            std::string label = !query.automation_id.empty()
                ? ("automation_id=" + query.automation_id)
                : !query.name.empty()
                    ? ("name=" + query.name)
                    : "control_type=" + std::to_string(query.control_type.value_or(0));
            set_error("find: timeout waiting for element (" + label + ")");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        delay = std::min(delay * 2, 250);
    }
    return result;
}

bool UiaSession::click(const Element& el)
{
    if (!el.valid()) {
        set_error("click: element is invalid");
        return false;
    }

    // Prefer InvokePattern -- programmatic activation that doesn't need
    // a focused window or specific cursor position.
    IUnknown* pat = nullptr;
    if (SUCCEEDED(el.raw()->GetCurrentPattern(UIA_InvokePatternId, &pat)) && pat) {
        IUIAutomationInvokePattern* inv = nullptr;
        pat->QueryInterface(__uuidof(IUIAutomationInvokePattern),
                            reinterpret_cast<void**>(&inv));
        pat->Release();
        if (inv) {
            HRESULT hr = inv->Invoke();
            inv->Release();
            if (SUCCEEDED(hr)) return true;
        }
    }
    // SelectionItem fallback covers toggle buttons + tabs that expose
    // Select() instead of Invoke().
    if (SUCCEEDED(el.raw()->GetCurrentPattern(UIA_SelectionItemPatternId, &pat)) && pat) {
        IUIAutomationSelectionItemPattern* sel = nullptr;
        pat->QueryInterface(__uuidof(IUIAutomationSelectionItemPattern),
                            reinterpret_cast<void**>(&sel));
        pat->Release();
        if (sel) {
            HRESULT hr = sel->Select();
            sel->Release();
            if (SUCCEEDED(hr)) return true;
        }
    }
    // Toggle pattern covers wxToggleButton (mode switcher).
    if (SUCCEEDED(el.raw()->GetCurrentPattern(UIA_TogglePatternId, &pat)) && pat) {
        IUIAutomationTogglePattern* tog = nullptr;
        pat->QueryInterface(__uuidof(IUIAutomationTogglePattern),
                            reinterpret_cast<void**>(&tog));
        pat->Release();
        if (tog) {
            HRESULT hr = tog->Toggle();
            tog->Release();
            if (SUCCEEDED(hr)) return true;
        }
    }

    // Fallback: synthesise a mouse click at the bounding-rect centre.
    RECT r{};
    if (FAILED(el.raw()->get_CurrentBoundingRectangle(&r))) {
        set_error("click: no Invoke pattern and no bounding rectangle");
        return false;
    }
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;
    INPUT inputs[3]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = (cx * 65535) / GetSystemMetrics(SM_CXSCREEN);
    inputs[0].mi.dy = (cy * 65535) / GetSystemMetrics(SM_CYSCREEN);
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[2].type = INPUT_MOUSE;
    inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    UINT n = SendInput(3, inputs, sizeof(INPUT));
    if (n != 3) {
        set_error("click: SendInput fallback failed");
        return false;
    }
    return true;
}

bool UiaSession::type_text(const Element& el, const std::string& text)
{
    if (!el.valid()) {
        set_error("type_text: element is invalid");
        return false;
    }

    // UIA's SetFocus is async and not always honoured by RichEdit. Pull the
    // underlying HWND, force focus via Win32 SetForegroundWindow + SetFocus,
    // then PostMessage WM_CHAR per character. This goes through the standard
    // Win32 message pump that wx's event handlers listen on -- both wx's
    // EVT_TEXT (so the wxTextCtrl::GetValue() backing model updates) and
    // EVT_KEY_DOWN (so Enter triggers submit) fire correctly.
    UIA_HWND native = nullptr;
    el.raw()->get_CurrentNativeWindowHandle(&native);
    HWND target = reinterpret_cast<HWND>(native);
    if (!target) target = target_hwnd_;

    if (target) {
        // Top-level window first, then the actual control.
        HWND root = GetAncestor(target, GA_ROOT);
        if (root) SetForegroundWindow(root);
        // AttachThreadInput so SetFocus succeeds from our thread to the
        // target's thread. (SetFocus only works on windows owned by the
        // calling thread otherwise.)
        DWORD tid = GetWindowThreadProcessId(target, nullptr);
        DWORD my  = GetCurrentThreadId();
        bool attached = (tid != my) && AttachThreadInput(my, tid, TRUE);
        SetFocus(target);
        if (attached) AttachThreadInput(my, tid, FALSE);
    } else {
        el.raw()->SetFocus();
    }

    std::wstring w = utf8_to_wide(text);
    if (target) {
        // PostMessage path -- delivers WM_CHAR directly to the focused
        // control. Reliable when SendInput's foreground-window heuristic
        // misses (e.g. if another app's window stole focus mid-script).
        for (wchar_t ch : w) {
            PostMessageW(target, WM_CHAR, (WPARAM)ch, 0);
        }
        return true;
    }

    // No HWND -- fall back to SendInput (won't work for hwnd-less elements
    // like WebView2 content, but the user will see the error then).
    for (wchar_t ch : w) {
        INPUT in[2]{};
        in[0].type       = INPUT_KEYBOARD;
        in[0].ki.wScan   = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        UINT n = SendInput(2, in, sizeof(INPUT));
        if (n != 2) {
            set_error("type_text: SendInput failed mid-string");
            return false;
        }
    }
    return true;
}

bool UiaSession::press_key(const std::string& key_name, const std::string& modifiers)
{
    static const std::array<std::pair<std::string, WORD>, 24> keys = {{
        {"ENTER",     VK_RETURN},
        {"RETURN",    VK_RETURN},
        {"ESC",       VK_ESCAPE},
        {"ESCAPE",    VK_ESCAPE},
        {"TAB",       VK_TAB},
        {"SPACE",     VK_SPACE},
        {"BACKSPACE", VK_BACK},
        {"UP",        VK_UP},
        {"DOWN",      VK_DOWN},
        {"LEFT",      VK_LEFT},
        {"RIGHT",     VK_RIGHT},
        {"COMMA",     VK_OEM_COMMA},
        {"PERIOD",    VK_OEM_PERIOD},
        {"F1",        VK_F1},
        {"F2",        VK_F2},
        {"F3",        VK_F3},
        {"F4",        VK_F4},
        {"F5",        VK_F5},
        {"F6",        VK_F6},
        {"F7",        VK_F7},
        {"F8",        VK_F8},
        {"F9",        VK_F9},
        {"F10",       VK_F10},
        {"F11",       VK_F11},
    }};
    auto it = std::find_if(keys.begin(), keys.end(),
        [&](const auto& p) { return p.first == key_name; });
    if (it == keys.end()) {
        set_error("press_key: unknown key '" + key_name + "'");
        return false;
    }

    // Parse modifiers ("CTRL", "ALT", "SHIFT") -- accept comma or plus
    // separators; case-insensitive.
    std::vector<WORD> mods;
    {
        std::string m;
        m.reserve(modifiers.size());
        for (char c : modifiers) m.push_back((char)std::toupper((unsigned char)c));
        size_t start = 0;
        while (start < m.size()) {
            size_t pos = m.find_first_of(",+", start);
            std::string tok = m.substr(start, pos - start);
            // trim spaces
            while (!tok.empty() && std::isspace((unsigned char)tok.front())) tok.erase(tok.begin());
            while (!tok.empty() && std::isspace((unsigned char)tok.back()))  tok.pop_back();
            if (tok == "CTRL" || tok == "CONTROL")     mods.push_back(VK_CONTROL);
            else if (tok == "ALT")                      mods.push_back(VK_MENU);
            else if (tok == "SHIFT")                    mods.push_back(VK_SHIFT);
            else if (!tok.empty()) {
                set_error("press_key: unknown modifier '" + tok + "'");
                return false;
            }
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
    }

    std::vector<INPUT> inputs;
    inputs.reserve(mods.size() * 2 + 2);
    for (WORD m : mods) {
        INPUT in{};
        in.type   = INPUT_KEYBOARD;
        in.ki.wVk = m;
        inputs.push_back(in);
    }
    INPUT down{};
    down.type   = INPUT_KEYBOARD;
    down.ki.wVk = it->second;
    inputs.push_back(down);
    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(up);
    // Release modifiers in reverse order.
    for (auto rit = mods.rbegin(); rit != mods.rend(); ++rit) {
        INPUT in{};
        in.type      = INPUT_KEYBOARD;
        in.ki.wVk    = *rit;
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(in);
    }

    UINT n = SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
    if (n != inputs.size()) {
        set_error("press_key: SendInput failed");
        return false;
    }
    return true;
}

bool UiaSession::press_key_to(const Element& target,
                              const std::string& key_name,
                              const std::string& modifiers)
{
    if (!target.valid()) {
        set_error("press_key_to: target element invalid");
        return false;
    }

    static const std::array<std::pair<std::string, WORD>, 24> keys = {{
        {"ENTER",     VK_RETURN},
        {"RETURN",    VK_RETURN},
        {"ESC",       VK_ESCAPE},
        {"ESCAPE",    VK_ESCAPE},
        {"TAB",       VK_TAB},
        {"SPACE",     VK_SPACE},
        {"BACKSPACE", VK_BACK},
        {"UP",        VK_UP},
        {"DOWN",      VK_DOWN},
        {"LEFT",      VK_LEFT},
        {"RIGHT",     VK_RIGHT},
        {"COMMA",     VK_OEM_COMMA},
        {"PERIOD",    VK_OEM_PERIOD},
        {"F1",        VK_F1},
        {"F2",        VK_F2},
        {"F3",        VK_F3},
        {"F4",        VK_F4},
        {"F5",        VK_F5},
        {"F6",        VK_F6},
        {"F7",        VK_F7},
        {"F8",        VK_F8},
        {"F9",        VK_F9},
        {"F10",       VK_F10},
        {"F11",       VK_F11},
    }};
    auto it = std::find_if(keys.begin(), keys.end(),
        [&](const auto& p) { return p.first == key_name; });
    if (it == keys.end()) {
        set_error("press_key_to: unknown key '" + key_name + "'");
        return false;
    }

    UIA_HWND native = nullptr;
    target.raw()->get_CurrentNativeWindowHandle(&native);
    HWND hwnd = reinterpret_cast<HWND>(native);

    // No HWND -- the element is a UIA-only abstraction (e.g. a Document
    // inside WebView2's DOM tree). Fall back to SendInput against whatever
    // currently has focus. Acceptable because the typical use is "type then
    // press Enter" where the type step already focused a real HWND.
    if (!hwnd) return press_key(key_name, modifiers);

    // Modifiers are tricky over PostMessage -- WM_KEYDOWN with the modifier
    // doesn't update Win32's modifier state for the receiving thread. For
    // the Enter / Esc / Tab case we don't need modifiers. Punt: if modifiers
    // are requested, fall through to the SendInput path on the focused
    // window.
    if (!modifiers.empty()) return press_key(key_name, modifiers);

    PostMessageW(hwnd, WM_KEYDOWN, (WPARAM)it->second, 0);
    PostMessageW(hwnd, WM_KEYUP,   (WPARAM)it->second, 0);
    return true;
}

std::string UiaSession::read_text(const Element& el)
{
    if (!el.valid()) return {};

    // ValuePattern -- edits and value-bearing controls.
    IUnknown* pat = nullptr;
    if (SUCCEEDED(el.raw()->GetCurrentPattern(UIA_ValuePatternId, &pat)) && pat) {
        IUIAutomationValuePattern* vp = nullptr;
        pat->QueryInterface(__uuidof(IUIAutomationValuePattern),
                            reinterpret_cast<void**>(&vp));
        pat->Release();
        if (vp) {
            BSTR b = nullptr;
            vp->get_CurrentValue(&b);
            vp->Release();
            if (b) {
                std::string s = bstr_to_utf8(b);
                SysFreeString(b);
                return s;
            }
        }
    }
    // TextPattern -- documents / WebView content.
    if (SUCCEEDED(el.raw()->GetCurrentPattern(UIA_TextPatternId, &pat)) && pat) {
        IUIAutomationTextPattern* tp = nullptr;
        pat->QueryInterface(__uuidof(IUIAutomationTextPattern),
                            reinterpret_cast<void**>(&tp));
        pat->Release();
        if (tp) {
            IUIAutomationTextRange* range = nullptr;
            tp->get_DocumentRange(&range);
            tp->Release();
            if (range) {
                BSTR b = nullptr;
                range->GetText(-1, &b);
                range->Release();
                if (b) {
                    std::string s = bstr_to_utf8(b);
                    SysFreeString(b);
                    return s;
                }
            }
        }
    }
    return el.name();
}

std::string UiaSession::read_all_text(const Element& root)
{
    if (!ready() || !root.valid()) return {};

    // FindAll(TrueCondition) under the root scoped to Subtree, filter to
    // Text + Edit + Document control types.
    IUIAutomationCondition* true_cond = nullptr;
    automation_->CreateTrueCondition(&true_cond);

    IUIAutomationElementArray* arr = nullptr;
    HRESULT hr = root.raw()->FindAll(TreeScope_Subtree, true_cond, &arr);
    if (true_cond) true_cond->Release();
    if (FAILED(hr) || !arr) return {};

    int len = 0;
    arr->get_Length(&len);
    std::string out;
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el_raw = nullptr;
        if (FAILED(arr->GetElement(i, &el_raw)) || !el_raw) continue;
        Element el(el_raw);
        int ct = el.control_type();
        std::string s;
        if (ct == UIA_TextControlTypeId || ct == UIA_EditControlTypeId ||
            ct == UIA_DocumentControlTypeId) {
            // Real text-bearing controls: read full content via the
            // Text/Value pattern (handles multi-line, document-scale text).
            s = read_text(el);
        } else if (ct == UIA_ButtonControlTypeId ||
                   ct == UIA_HyperlinkControlTypeId ||
                   ct == UIA_CheckBoxControlTypeId ||
                   ct == UIA_ListItemControlTypeId ||
                   ct == UIA_TabItemControlTypeId ||
                   ct == UIA_GroupControlTypeId) {
            // Element types whose label-as-text lives in the Name property
            // (e.g. WebView2 surfaces HTML <button> / <a> labels here).
            // Keeps assertions like "Approve & execute" reachable without
            // needing the script to chase a specific control type.
            s = el.name();
        }
        if (!s.empty()) {
            if (!out.empty()) out += '\n';
            out += s;
        }
    }
    arr->Release();
    return out;
}

bool UiaSession::wait_for(std::function<bool()> predicate, int timeout_ms)
{
    auto start = std::chrono::steady_clock::now();
    int delay  = 50;
    while (true) {
        if (predicate()) return true;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        delay = std::min(delay * 2, 250);
    }
}

bool UiaSession::screenshot(const std::filesystem::path& out_path)
{
    if (!target_hwnd_) {
        set_error("screenshot: no window has been latched yet");
        return false;
    }
    RECT r{};
    if (!GetClientRect(target_hwnd_, &r)) {
        set_error("screenshot: GetClientRect failed");
        return false;
    }
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        set_error("screenshot: window has zero size");
        return false;
    }

    HDC hdc_window = GetDC(target_hwnd_);
    HDC hdc_mem    = CreateCompatibleDC(hdc_window);
    HBITMAP bmp    = CreateCompatibleBitmap(hdc_window, w, h);
    HGDIOBJ old    = SelectObject(hdc_mem, bmp);

    // PW_RENDERFULLCONTENT renders WebView2 / DComp content; without it
    // the chat WebView appears as a black rectangle.
    BOOL ok = PrintWindow(target_hwnd_, hdc_mem, PW_RENDERFULLCONTENT);
    SelectObject(hdc_mem, old);

    bool wrote = false;
    if (ok) wrote = save_hbitmap_as_png(bmp, out_path);

    DeleteObject(bmp);
    DeleteDC(hdc_mem);
    ReleaseDC(target_hwnd_, hdc_window);

    if (!ok)     set_error("screenshot: PrintWindow failed");
    if (!wrote)  set_error("screenshot: failed to write " + out_path.string());
    return ok && wrote;
}

static const char* control_type_name(int ct)
{
    switch (ct) {
        case UIA_ButtonControlTypeId:        return "Button";
        case UIA_CheckBoxControlTypeId:      return "CheckBox";
        case UIA_ComboBoxControlTypeId:      return "ComboBox";
        case UIA_EditControlTypeId:          return "Edit";
        case UIA_HyperlinkControlTypeId:     return "Hyperlink";
        case UIA_ImageControlTypeId:         return "Image";
        case UIA_ListControlTypeId:          return "List";
        case UIA_ListItemControlTypeId:      return "ListItem";
        case UIA_MenuControlTypeId:          return "Menu";
        case UIA_MenuBarControlTypeId:       return "MenuBar";
        case UIA_MenuItemControlTypeId:      return "MenuItem";
        case UIA_PaneControlTypeId:          return "Pane";
        case UIA_RadioButtonControlTypeId:   return "RadioButton";
        case UIA_ScrollBarControlTypeId:     return "ScrollBar";
        case UIA_SliderControlTypeId:        return "Slider";
        case UIA_SpinnerControlTypeId:       return "Spinner";
        case UIA_StatusBarControlTypeId:     return "StatusBar";
        case UIA_TabControlTypeId:           return "Tab";
        case UIA_TabItemControlTypeId:       return "TabItem";
        case UIA_TextControlTypeId:          return "Text";
        case UIA_ToolBarControlTypeId:       return "ToolBar";
        case UIA_ToolTipControlTypeId:       return "ToolTip";
        case UIA_TreeControlTypeId:          return "Tree";
        case UIA_TreeItemControlTypeId:      return "TreeItem";
        case UIA_CustomControlTypeId:        return "Custom";
        case UIA_GroupControlTypeId:         return "Group";
        case UIA_ThumbControlTypeId:         return "Thumb";
        case UIA_DataGridControlTypeId:      return "DataGrid";
        case UIA_DataItemControlTypeId:      return "DataItem";
        case UIA_DocumentControlTypeId:      return "Document";
        case UIA_SplitButtonControlTypeId:   return "SplitButton";
        case UIA_WindowControlTypeId:        return "Window";
        case UIA_TitleBarControlTypeId:      return "TitleBar";
        default: return "?";
    }
}

static void dump_recurse(IUIAutomationElement* el, int depth, int max_depth,
                         std::ostringstream& out, IUIAutomation* uia)
{
    if (!el || depth > max_depth) return;
    int ct = 0;
    el->get_CurrentControlType(&ct);
    BSTR bname = nullptr; el->get_CurrentName(&bname);
    BSTR baid  = nullptr; el->get_CurrentAutomationId(&baid);
    std::string name = bstr_to_utf8(bname); if (bname) SysFreeString(bname);
    std::string aid  = bstr_to_utf8(baid);  if (baid)  SysFreeString(baid);
    for (int i = 0; i < depth; ++i) out << "  ";
    out << "[" << control_type_name(ct) << "]"
        << "  name=\"" << name << "\""
        << "  aid=\"" << aid << "\"\n";

    if (depth == max_depth) return;
    IUIAutomationCondition* tcond = nullptr;
    uia->CreateTrueCondition(&tcond);
    IUIAutomationElementArray* arr = nullptr;
    HRESULT hr = el->FindAll(TreeScope_Children, tcond, &arr);
    if (tcond) tcond->Release();
    if (FAILED(hr) || !arr) return;
    int len = 0;
    arr->get_Length(&len);
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* child = nullptr;
        if (SUCCEEDED(arr->GetElement(i, &child)) && child) {
            dump_recurse(child, depth + 1, max_depth, out, uia);
            child->Release();
        }
    }
    arr->Release();
}

std::string UiaSession::dump_tree(const Element* root, int max_depth)
{
    if (!ready()) return "(uia not ready)";
    std::ostringstream out;
    if (root && root->valid()) {
        dump_recurse(root->raw(), 0, max_depth, out, automation_);
    } else {
        for (HWND hwnd : enum_process_top_level(process_id_)) {
            IUIAutomationElement* el = nullptr;
            if (SUCCEEDED(automation_->ElementFromHandle(hwnd, &el)) && el) {
                wchar_t title[256] = { 0 };
                GetWindowTextW(hwnd, title, 255);
                out << "=== HWND " << (void*)hwnd << " title=\""
                    << wide_to_utf8(title) << "\" ===\n";
                dump_recurse(el, 0, max_depth, out, automation_);
                el->Release();
            }
        }
    }
    return out.str();
}

void UiaSession::close(int wait_ms)
{
    if (!process_handle_) return;

    // Try graceful WM_CLOSE first so OnExit hooks run.
    if (target_hwnd_) {
        PostMessageW(target_hwnd_, WM_CLOSE, 0, 0);
    }
    DWORD wait = WaitForSingleObject(process_handle_, wait_ms);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process_handle_, 1);
        WaitForSingleObject(process_handle_, 2000);
    }
    CloseHandle(process_handle_);
    process_handle_ = nullptr;
    process_id_     = 0;
    target_hwnd_    = nullptr;
}

// ---------------------------------------------------------------------------
// PNG writer via WIC
// ---------------------------------------------------------------------------

bool save_hbitmap_as_png(HBITMAP bitmap, const std::filesystem::path& out_path)
{
    BITMAP bm{};
    if (!GetObjectW(bitmap, sizeof(bm), &bm)) return false;

    // CoCreateInstance the WIC factory. ApartmentThreaded was already set
    // by UiaSession; if someone calls this without a session active they
    // need to initialise COM themselves.
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return false;

    IWICBitmap* wic_bmp = nullptr;
    hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr,
                                          WICBitmapIgnoreAlpha, &wic_bmp);
    if (FAILED(hr) || !wic_bmp) { factory->Release(); return false; }

    IWICStream* stream = nullptr;
    factory->CreateStream(&stream);
    if (!stream) { wic_bmp->Release(); factory->Release(); return false; }

    std::wstring w_path = out_path.wstring();
    if (FAILED(stream->InitializeFromFilename(w_path.c_str(), GENERIC_WRITE))) {
        stream->Release(); wic_bmp->Release(); factory->Release();
        return false;
    }

    IWICBitmapEncoder* enc = nullptr;
    factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (!enc) {
        stream->Release(); wic_bmp->Release(); factory->Release();
        return false;
    }
    enc->Initialize(stream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode* frame = nullptr;
    enc->CreateNewFrame(&frame, nullptr);
    if (!frame) {
        enc->Release(); stream->Release(); wic_bmp->Release(); factory->Release();
        return false;
    }
    frame->Initialize(nullptr);
    frame->SetSize(bm.bmWidth, bm.bmHeight);

    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&pf);
    frame->WriteSource(wic_bmp, nullptr);
    frame->Commit();
    enc->Commit();

    frame->Release();
    enc->Release();
    stream->Release();
    wic_bmp->Release();
    factory->Release();
    return true;
}

} // namespace locus::uia
