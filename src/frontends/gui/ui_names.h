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
inline constexpr const char* kAboutDialog    = "locus.about.dialog";

// --- Chat panel -----------------------------------------------------------
inline constexpr const char* kChatPanel      = "locus.chat.panel";
inline constexpr const char* kChatWebView    = "locus.chat.webview";
inline constexpr const char* kChatInput      = "locus.chat.input";
inline constexpr const char* kChatCompactBtn = "locus.chat.compact_btn";
inline constexpr const char* kChatAutoCompactToggle = "locus.chat.auto_compact_cb";
inline constexpr const char* kChatStopBtn    = "locus.chat.stop_btn";
inline constexpr const char* kChatUndoBtn    = "locus.chat.undo_btn";
inline constexpr const char* kChatModeChat   = "locus.chat.mode_chat";
inline constexpr const char* kChatModePlan   = "locus.chat.mode_plan";
inline constexpr const char* kChatModeExec   = "locus.chat.mode_execute";
inline constexpr const char* kChatCtxLabel   = "locus.chat.ctx_label";
inline constexpr const char* kChatPlanChip       = "locus.chat.plan_chip";
inline constexpr const char* kChatCommitChip     = "locus.chat.commit_chip";
// S5.Z task 6 -- per-session compactions counter chip; hidden when N == 0.
inline constexpr const char* kChatCompactedChip  = "locus.chat.compacted_chip";
inline constexpr const char* kChatPresetChip     = "locus.chat.preset_chip";
inline constexpr const char* kChatPresetChoice   = "locus.chat.preset_choice";
inline constexpr const char* kChatSlashPopup     = "locus.chat.slash_popup";
inline constexpr const char* kChatMentionPopup   = "locus.chat.mention_popup";

// Chat find-in-conversation bar (S5.Z task 2).
inline constexpr const char* kChatFindBar        = "locus.chat.find_bar";
inline constexpr const char* kChatFindInput      = "locus.chat.find_input";
inline constexpr const char* kChatFindCounter    = "locus.chat.find_counter";
inline constexpr const char* kChatFindPrevBtn    = "locus.chat.find_prev_btn";
inline constexpr const char* kChatFindNextBtn    = "locus.chat.find_next_btn";
inline constexpr const char* kChatFindCaseToggle = "locus.chat.find_case_toggle";
inline constexpr const char* kChatFindCloseBtn   = "locus.chat.find_close_btn";
inline constexpr const char* kChatFindBtn        = "locus.chat.find_btn";

// --- Activity panel -------------------------------------------------------
inline constexpr const char* kActivityPanel    = "locus.activity.panel";
inline constexpr const char* kActivityNotebook = "locus.activity.notebook";

// --- Multi-tab chat notebook (S5.I) ---------------------------------------
inline constexpr const char* kChatNotebook    = "locus.chat.notebook";

// --- Manage Sessions dialog (S5.I) ----------------------------------------
inline constexpr const char* kManageSessionsDialog    = "locus.manage_sessions.dialog";
inline constexpr const char* kManageSessionsList      = "locus.manage_sessions.list";
inline constexpr const char* kManageSessionsBtnOpen   = "locus.manage_sessions.btn_open";
inline constexpr const char* kManageSessionsBtnRename = "locus.manage_sessions.btn_rename";
inline constexpr const char* kManageSessionsBtnDelete = "locus.manage_sessions.btn_delete";
inline constexpr const char* kManageSessionsBtnClose  = "locus.manage_sessions.btn_close";

// --- Terminal panel (S5.B) ------------------------------------------------
inline constexpr const char* kTerminalPanel    = "locus.terminal.panel";
inline constexpr const char* kTerminalNotebook = "locus.terminal.notebook";
// S5.Z task 4 -- per-bg-tab stdin input. Actual name is this prefix + the
// numeric bg id (e.g. "locus.terminal.stdin_input.7"), since the panel can
// host multiple bg tabs simultaneously.
inline constexpr const char* kTerminalStdinInputPrefix = "locus.terminal.stdin_input.";

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
inline constexpr const char* kSettingsTabNotifications = "locus.settings.tab.notifications";
inline constexpr const char* kSettingsSaveAsGlobalBtn = "locus.settings.save_as_global_btn";

// Settings -> LLM tab sub-controls (S5.L Phase A extension).
inline constexpr const char* kSettingsLlmEndpoint     = "locus.settings.llm.endpoint";
inline constexpr const char* kSettingsLlmModel        = "locus.settings.llm.model";
inline constexpr const char* kSettingsLlmTemperature  = "locus.settings.llm.temperature";
inline constexpr const char* kSettingsLlmContextLimit = "locus.settings.llm.context_limit";
inline constexpr const char* kSettingsLlmMaxTokens    = "locus.settings.llm.max_tokens";
inline constexpr const char* kSettingsLlmTimeoutSeconds = "locus.settings.llm.timeout_seconds";
inline constexpr const char* kSettingsLlmPresetChoice = "locus.settings.llm.preset_choice";
inline constexpr const char* kSettingsLlmPresetApplyBtn = "locus.settings.llm.preset_apply_btn";
inline constexpr const char* kSettingsLlmToolFormat   = "locus.settings.llm.tool_format";

// Settings -> Tool Approvals tab.
inline constexpr const char* kSettingsApprovalsList         = "locus.settings.approvals.list";
inline constexpr const char* kSettingsApprovalsPresetChoice = "locus.settings.approvals.preset_choice";
// Per-tool dropdowns are named "locus.settings.approvals.choice.<tool_name>"
// composed at construction; not a constant here.

// Settings -> MCP tab.
inline constexpr const char* kSettingsMcpList         = "locus.settings.mcp.list";
inline constexpr const char* kSettingsMcpRestartBtn   = "locus.settings.mcp.restart_btn";
inline constexpr const char* kSettingsMcpOpenJsonBtn  = "locus.settings.mcp.open_json_btn";
inline constexpr const char* kSettingsMcpTrustCheck   = "locus.settings.mcp.trust_check";

// --- Memory Bank panel (S5.K) ---------------------------------------------
inline constexpr const char* kMemoryBankPanel        = "locus.memory_bank.panel";
inline constexpr const char* kMemoryBankList         = "locus.memory_bank.list";
inline constexpr const char* kMemoryBankDetail       = "locus.memory_bank.detail";
inline constexpr const char* kMemoryBankSearch       = "locus.memory_bank.search";
inline constexpr const char* kMemoryBankSourceChoice = "locus.memory_bank.source_choice";
inline constexpr const char* kMemoryBankTagChoice    = "locus.memory_bank.tag_choice";
inline constexpr const char* kMemoryBankPinnedOnly   = "locus.memory_bank.pinned_only";
inline constexpr const char* kMemoryBankShowDeleted  = "locus.memory_bank.show_deleted";
inline constexpr const char* kMemoryBankClearBtn     = "locus.memory_bank.clear_btn";
inline constexpr const char* kMemoryBankDeleteBtn    = "locus.memory_bank.delete_btn";
inline constexpr const char* kMemoryBankPinBtn       = "locus.memory_bank.pin_btn";
inline constexpr const char* kMemoryBankTagBtn       = "locus.memory_bank.tag_btn";
inline constexpr const char* kMemoryBankSaveBtn      = "locus.memory_bank.save_btn";
inline constexpr const char* kMemoryBankRestoreBtn   = "locus.memory_bank.restore_btn";

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
inline constexpr const char* kCompactionSaveBtn      = "locus.compaction.save_btn";
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
