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
inline constexpr const char* kChatPlanChip       = "locus.chat.plan_chip";
inline constexpr const char* kChatCommitChip     = "locus.chat.commit_chip";
inline constexpr const char* kChatSlashPopup     = "locus.chat.slash_popup";
inline constexpr const char* kChatMentionPopup   = "locus.chat.mention_popup";

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
inline constexpr const char* kSettingsTabCapabilities = "locus.settings.tab.capabilities";
inline constexpr const char* kSettingsSaveAsGlobalBtn = "locus.settings.save_as_global_btn";

// Settings -> LLM tab sub-controls (S5.L Phase A extension).
inline constexpr const char* kSettingsLlmEndpoint     = "locus.settings.llm.endpoint";
inline constexpr const char* kSettingsLlmModel        = "locus.settings.llm.model";
inline constexpr const char* kSettingsLlmTemperature  = "locus.settings.llm.temperature";
inline constexpr const char* kSettingsLlmContextLimit = "locus.settings.llm.context_limit";
inline constexpr const char* kSettingsLlmMaxTokens    = "locus.settings.llm.max_tokens";
inline constexpr const char* kSettingsLlmPresetChoice = "locus.settings.llm.preset_choice";
inline constexpr const char* kSettingsLlmPresetApplyBtn = "locus.settings.llm.preset_apply_btn";
inline constexpr const char* kSettingsLlmToolFormat   = "locus.settings.llm.tool_format";

// Settings -> Tool Approvals tab.
inline constexpr const char* kSettingsApprovalsList   = "locus.settings.approvals.list";
// Per-tool dropdowns are named "locus.settings.approvals.choice.<tool_name>"
// composed at construction; not a constant here.

// Settings -> MCP tab.
inline constexpr const char* kSettingsMcpList         = "locus.settings.mcp.list";
inline constexpr const char* kSettingsMcpRestartBtn   = "locus.settings.mcp.restart_btn";
inline constexpr const char* kSettingsMcpOpenJsonBtn  = "locus.settings.mcp.open_json_btn";
inline constexpr const char* kSettingsMcpTrustCheck   = "locus.settings.mcp.trust_check";

// --- Capabilities first-open dialog (S5.A) --------------------------------
inline constexpr const char* kCapabilitiesDialog    = "locus.capabilities.dialog";
inline constexpr const char* kCapabilityBg          = "locus.capabilities.cb_background";
inline constexpr const char* kCapabilitySemantic    = "locus.capabilities.cb_semantic";
inline constexpr const char* kCapabilityCode        = "locus.capabilities.cb_code";
inline constexpr const char* kCapabilityMemory      = "locus.capabilities.cb_memory";
inline constexpr const char* kCapabilityWeb         = "locus.capabilities.cb_web";

// --- Compaction dialog ----------------------------------------------------
inline constexpr const char* kCompactionDialog       = "locus.compaction.dialog";
inline constexpr const char* kCompactionStrategyB    = "locus.compaction.strategy_b";
inline constexpr const char* kCompactionStrategyC    = "locus.compaction.strategy_c";
inline constexpr const char* kCompactionTurnsSlider  = "locus.compaction.turns_slider";
inline constexpr const char* kCompactionPreviewList  = "locus.compaction.preview_list";

// --- Tool approval dialog -------------------------------------------------
inline constexpr const char* kToolApprovalDialog        = "locus.tool_approval.dialog";
inline constexpr const char* kToolApprovalArgsView      = "locus.tool_approval.args_view";
inline constexpr const char* kToolApprovalApproveBtn    = "locus.tool_approval.approve_btn";
inline constexpr const char* kToolApprovalRejectBtn     = "locus.tool_approval.reject_btn";
inline constexpr const char* kToolApprovalModifyBtn     = "locus.tool_approval.modify_btn";
inline constexpr const char* kToolApprovalSafetyBanner  = "locus.tool_approval.safety_banner";
inline constexpr const char* kToolApprovalAskInput      = "locus.tool_approval.ask_input";
inline constexpr const char* kToolApprovalNameLabel     = "locus.tool_approval.name_label";

} // namespace locus::ui_names
