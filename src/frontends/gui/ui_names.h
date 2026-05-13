#pragma once

// S5.L -- automation names for wx controls.
//
// Each `kName*` string is the value passed to `wxWindow::SetName()` so the
// UI Automation driver in tests/ui_automation/ can find the control without
// guessing layout indices. The strings double as UIA AutomationId, so the
// driver and the GUI agree on identifiers from one place.
//
// Convention: prefix `locus.` keeps these out of collision with wx's own
// internal names; the dot-segment after that follows the panel they live on.
// Add a new entry whenever a script needs to address a widget the script
// can't otherwise reach by control type + parent.

namespace locus::ui_names {

// --- Main frame -----------------------------------------------------------
inline constexpr const char* kMainFrame      = "locus.main_frame";

// --- Chat panel -----------------------------------------------------------
inline constexpr const char* kChatPanel      = "locus.chat.panel";
inline constexpr const char* kChatWebView    = "locus.chat.webview";
inline constexpr const char* kChatInput      = "locus.chat.input";
inline constexpr const char* kChatCompactBtn = "locus.chat.compact_btn";
inline constexpr const char* kChatStopBtn    = "locus.chat.stop_btn";
inline constexpr const char* kChatUndoBtn    = "locus.chat.undo_btn";
inline constexpr const char* kChatModeChat   = "locus.chat.mode_chat";
inline constexpr const char* kChatModePlan   = "locus.chat.mode_plan";
inline constexpr const char* kChatModeExec   = "locus.chat.mode_execute";
inline constexpr const char* kChatCtxLabel   = "locus.chat.ctx_label";

// --- Activity panel -------------------------------------------------------
inline constexpr const char* kActivityPanel    = "locus.activity.panel";
inline constexpr const char* kActivityNotebook = "locus.activity.notebook";

// --- Terminal panel (S5.B) ------------------------------------------------
inline constexpr const char* kTerminalPanel    = "locus.terminal.panel";
inline constexpr const char* kTerminalNotebook = "locus.terminal.notebook";

// --- File tree panel ------------------------------------------------------
inline constexpr const char* kFileTreePanel = "locus.filetree.panel";
inline constexpr const char* kFileTreeCtrl  = "locus.filetree.tree";

// --- Settings dialog ------------------------------------------------------
inline constexpr const char* kSettingsDialog          = "locus.settings.dialog";
inline constexpr const char* kSettingsNotebook        = "locus.settings.notebook";
inline constexpr const char* kSettingsTabLlm          = "locus.settings.tab.llm";
inline constexpr const char* kSettingsTabIndex        = "locus.settings.tab.index";
inline constexpr const char* kSettingsTabApprovals    = "locus.settings.tab.approvals";
inline constexpr const char* kSettingsTabMcp          = "locus.settings.tab.mcp";

} // namespace locus::ui_names
