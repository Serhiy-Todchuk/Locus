#include "chat_panel.h"
#include "chat/chat_footer_chips.h"
#include "chat/chat_link_handler.h"
#include "chat/chat_popups.h"
#include "chat/chat_stream_renderer.h"
#include "chat/chat_util.h"
#include "chat_html_template.h"
#include "diff_renderer.h"
#include "locus_accessible.h"
#include "markdown.h"
#include "theme.h"
#include "ui_names.h"

#include "../../agent/conversation.h"
#include "../../agent/mention_parser.h"
#include "../../agent/metrics.h"
#include "../../llm/token_counter.h"
#include "../../tools/tool.h"

#include <spdlog/spdlog.h>

#include <wx/webview.h>

#include <algorithm>
#include <filesystem>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace locus {

// Timer interval for flushing token buffer to WebView (ms).
static constexpr int k_flush_interval_ms = 33;  // ~30fps

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ChatPanel::ChatPanel(wxWindow* parent,
                     std::function<void(const std::string&)> on_send,
                     std::function<void()> on_compact,
                     std::function<void()> on_stop,
                     std::function<void()> on_undo,
                     std::function<void(AgentMode)> on_mode_pick,
                     std::function<void(const std::string&)> on_plan_decision)
    : wxPanel(parent, wxID_ANY)
    , on_send_(std::move(on_send))
    , on_compact_(std::move(on_compact))
    , on_stop_(std::move(on_stop))
    , on_undo_(std::move(on_undo))
    , on_mode_pick_(std::move(on_mode_pick))
    , on_plan_decision_(std::move(on_plan_decision))
    , flush_timer_(this)
{
    SetName(ui_names::kChatPanel);
    gui::apply_locus_accessible_name(this);

    create_webview();
    create_input();
    create_find_bar();

    // Footer chips must be created before create_footer() wires the sizer.
    footer_chips_ = std::make_unique<ChatFooterChips>(this);

    create_footer();

    // Collaborators that need the input widget.
    popups_ = std::make_unique<ChatPopups>(this, input_);

    // Streaming renderer shares message_id_ by reference.
    renderer_ = std::make_unique<ChatStreamRenderer>(
        [this](const wxString& js) { run_script(js); },
        message_id_);

    // Link handler for locus:// URL dispatch.
    link_handler_ = std::make_unique<ChatLinkHandler>(
        [this](const wxString& js) { run_script(js); },
        on_plan_decision_,
        // S5.G -- per-message delete dispatch. Prompts the confirm dialog
        // (modal on this panel) and routes the history_id to LocusFrame's
        // on_delete_message_ closure (which calls AgentCore::delete_message).
        [this](int history_id) {
            if (!on_delete_message_) return;
            int answer = wxMessageBox(
                "Delete this message?\nSubsequent turns will no longer see it.",
                "Delete message",
                wxYES_NO | wxICON_QUESTION,
                this);
            if (answer != wxYES) return;
            on_delete_message_(history_id);
        });

    // S4.D mode switcher (Chat / Plan / Execute) -- mutually exclusive toggles.
    auto mk_toggle = [this](const wxString& label, AgentMode m,
                            const wxString& tip) {
        auto* btn = new wxToggleButton(this, wxID_ANY, label,
                                       wxDefaultPosition, wxDefaultSize,
                                       wxBU_EXACTFIT);
        btn->SetToolTip(tip);
        btn->Bind(wxEVT_TOGGLEBUTTON, [this, m, btn](wxCommandEvent&) {
            if (!btn->GetValue()) { btn->SetValue(true); return; }
            if (mode_chat_btn_    && mode_chat_btn_    != btn) mode_chat_btn_->SetValue(false);
            if (mode_plan_btn_    && mode_plan_btn_    != btn) mode_plan_btn_->SetValue(false);
            if (mode_execute_btn_ && mode_execute_btn_ != btn) mode_execute_btn_->SetValue(false);
            if (on_mode_pick_) on_mode_pick_(m);
        });
        return btn;
    };

    mode_chat_btn_ = mk_toggle("Chat", AgentMode::chat,
        "Default: full tool catalog, no plan workflow.");
    mode_chat_btn_->SetName(ui_names::kChatModeChat);
    gui::apply_locus_accessible_name(mode_chat_btn_);
    mode_plan_btn_ = mk_toggle("Plan", AgentMode::plan,
        "Plan mode: model proposes a structured plan; you Approve to execute.");
    mode_plan_btn_->SetName(ui_names::kChatModePlan);
    gui::apply_locus_accessible_name(mode_plan_btn_);
    mode_execute_btn_ = mk_toggle("Execute", AgentMode::execute,
        "Execute mode: full tool catalog plus mark_step_done. "
        "Usually entered automatically after Approve.");
    mode_execute_btn_->SetName(ui_names::kChatModeExec);
    gui::apply_locus_accessible_name(mode_execute_btn_);
    mode_chat_btn_->SetValue(true);

    // Attached-context chip row.
    attach_panel_ = new wxPanel(this, wxID_ANY);
    attach_label_ = new wxStaticText(attach_panel_, wxID_ANY, "");
    attach_close_ = new wxButton(attach_panel_, wxID_ANY, "x",
                                 wxDefaultPosition, wxSize(22, 22),
                                 wxBU_EXACTFIT);
    attach_close_->SetToolTip("Detach file from conversation context");
    attach_close_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_detach_) on_detach_();
    });
    auto* attach_sizer = new wxBoxSizer(wxHORIZONTAL);
    attach_sizer->Add(attach_label_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    attach_sizer->Add(attach_close_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
    attach_panel_->SetSizer(attach_sizer);
    attach_panel_->Hide();

    // Mode switcher row. Footer-declutter pass: Find lives in the centre
    // and the Permission combo lives on the right of this row so the
    // bottom footer only carries the ctx meter + round/compacted chips
    // + the right-edge action group (Auto / Compact / Undo / Submit).
    //   [Mode: Chat][Plan][Execute]   (stretch)   [Find]   (stretch)   [combo]
    auto* mode_row = new wxBoxSizer(wxHORIZONTAL);
    mode_row->Add(new wxStaticText(this, wxID_ANY, "Mode:"),
                  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    mode_row->Add(mode_chat_btn_,    0, wxRIGHT, 2);
    mode_row->Add(mode_plan_btn_,    0, wxRIGHT, 2);
    mode_row->Add(mode_execute_btn_, 0);
    mode_row->AddStretchSpacer();
    mode_row->Add(find_btn_,         0, wxALIGN_CENTER_VERTICAL);
    mode_row->AddStretchSpacer();
    mode_row->Add(preset_choice_,    0, wxALIGN_CENTER_VERTICAL);

    // Footer bar. Order: ctx_label (flex) -- round_chip -- compacted_btn --
    // stretch -- Auto / Compact / Undo / Submit. The user explicitly asked
    // for round to precede compacted so they read left-to-right "this turn
    // is at round 7/500, and you've compacted 12 times so far."
    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    footer->Add(footer_chips_->ctx_label(),   1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    footer->Add(footer_chips_->round_chip(),  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    footer->Add(footer_chips_->compacted_btn(), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    // S5.Z task 6 -- click the compacted button opens the session's archive
    // folder. wxLaunchDefaultApplication is unreliable for directories on
    // Windows (silently no-ops in some shell configurations); use the same
    // ShellExecuteW path FileTreePanel uses for files. Create the dir on
    // demand so the click works even before the very first archive write.
    footer_chips_->compacted_btn()->Bind(wxEVT_BUTTON,
        [this](wxCommandEvent&) {
            const wxString& dir = footer_chips_->compacted_archive_dir();
            if (dir.empty()) return;
            std::filesystem::path p(dir.ToStdWstring());
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
#ifdef _WIN32
            p.make_preferred();
            HINSTANCE r = ShellExecuteW(nullptr, L"open", p.wstring().c_str(),
                                        nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(r) <= 32) {
                spdlog::warn("ChatPanel: ShellExecute open failed for '{}' "
                             "(code {})",
                             p.string(), reinterpret_cast<INT_PTR>(r));
            }
#else
            if (!wxLaunchDefaultApplication(
                    wxString::FromUTF8(p.string())))
                spdlog::warn("ChatPanel: wxLaunchDefaultApplication failed "
                             "for '{}'", p.string());
#endif
        });
    footer->AddStretchSpacer();
    footer->Add(auto_compact_cb_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(compact_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(undo_btn_,    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    // S6.13 follow-up -- Commit-now sits just before Stop. Reserves no
    // horizontal space when hidden (wxSizer skips hidden items in default
    // layout) so the footer is unchanged in the common case.
    footer->Add(commit_btn_,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    footer->Add(stop_btn_,    0, wxALIGN_CENTER_VERTICAL);

    // Main vertical layout.
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(find_bar_,     0, wxEXPAND);
    sizer->Add(webview_,      1, wxEXPAND);
    sizer->Add(attach_panel_, 0, wxEXPAND | wxTOP | wxBOTTOM, 2);
    sizer->Add(mode_row,      0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 2);
    sizer->Add(input_,        0, wxEXPAND | wxTOP, 2);
    sizer->Add(footer,        0, wxEXPAND | wxALL, 4);
    SetSizer(sizer);

    Bind(wxEVT_TIMER, &ChatPanel::on_flush_timer, this);
}

ChatPanel::~ChatPanel() = default;

void ChatPanel::create_webview()
{
    webview_ = wxWebView::New(this, wxID_ANY);
    webview_->SetName(ui_names::kChatWebView);
    gui::apply_locus_accessible_name(webview_);
    webview_->SetPage(wxString::FromUTF8(build_chat_html()), "about:blank");

    webview_->Bind(wxEVT_WEBVIEW_NAVIGATING, &ChatPanel::on_webview_navigating, this);

    webview_->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent&) {
        if (page_ready_) return;
        page_ready_ = true;
        in_run_script_ = true;
        size_t n = 0;
        while (!pending_scripts_.empty()) {
            wxString next = pending_scripts_.front();
            pending_scripts_.erase(pending_scripts_.begin());
            webview_->RunScript(next);
            ++n;
        }
        in_run_script_ = false;
        spdlog::info("WebView page loaded, flushed {} queued scripts", n);
    });
}

void ChatPanel::create_input()
{
    input_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                            wxDefaultPosition, wxSize(-1, 60),
                            wxTE_MULTILINE | wxTE_PROCESS_ENTER | wxTE_RICH2);
    input_->SetName(ui_names::kChatInput);
    gui::apply_locus_accessible_name(input_);
    input_->SetMaxLength(0);
    input_->SetToolTip(
        "Type a message and press Enter to send.\n"
        "Shift+Enter inserts a newline. Type '/' for commands.");
    input_->SetBackgroundColour(theme::text_bg());
    input_->SetForegroundColour(theme::text_fg());
    input_->Bind(wxEVT_KEY_DOWN, &ChatPanel::on_input_key, this);
    input_->Bind(wxEVT_TEXT,     &ChatPanel::on_input_text, this);
    input_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& evt) {
        popups_->dismiss_all();
        evt.Skip();
    });
}

void ChatPanel::create_footer()
{
    // ctx_gauge_ and ctx_label_ live in footer_chips_; access via getters.
    // The UIA-accessible name must be set here since the footer chips don't
    // have a reference to ui_names.
    footer_chips_->ctx_label()->SetName(ui_names::kChatCtxLabel);
    gui::apply_locus_accessible_name(footer_chips_->ctx_label());

    compact_btn_ = new wxButton(this, wxID_ANY, "Compact",
                                wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    compact_btn_->SetName(ui_names::kChatCompactBtn);
    gui::apply_locus_accessible_name(compact_btn_);
    compact_btn_->SetToolTip("Open context compaction dialog");
    compact_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_compact_) on_compact_();
    });

    auto_compact_cb_ = new wxCheckBox(this, wxID_ANY, "Auto");
    auto_compact_cb_->SetName(ui_names::kChatAutoCompactToggle);
    gui::apply_locus_accessible_name(auto_compact_cb_);
    auto_compact_cb_->SetToolTip(
        "Auto-compact: when context usage crosses the configured threshold, "
        "run the saved compaction layers automatically before the next turn.\n"
        "Edit which layers run by clicking Compact and using its Save button.");
    auto_compact_cb_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& evt) {
        if (on_auto_compact_toggle_) on_auto_compact_toggle_(evt.IsChecked());
    });

    stop_btn_ = new wxButton(this, wxID_ANY, "Submit",
                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    stop_btn_->SetName(ui_names::kChatStopBtn);
    gui::apply_locus_accessible_name(stop_btn_);
    stop_btn_->Disable();
    stop_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (renderer_->is_streaming()) {
            bool has_text = !input_->GetValue().Trim().Trim(false).IsEmpty();
            if (has_text) submit_current_input();
            else if (on_stop_) on_stop_();
        } else {
            submit_current_input();
        }
    });

    // S6.13 follow-up -- Commit-now button. Sits inline next to Stop. Hidden
    // by default; surfaces when the reasoning watchdog trips (manual mode,
    // i.e. `agent.reasoning_auto_nudge=false`) and disappears again when the
    // round resolves (a tool call lands, the agent goes idle, or another
    // nudge fires). Click cancels the in-flight LLM stream and injects a
    // "Stop reasoning, commit to a tool call now" steering message instead
    // of aborting the whole turn (which is what the Stop button does).
    commit_btn_ = new wxButton(this, wxID_ANY, "Commit now",
                               wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    commit_btn_->SetName(ui_names::kChatCommitBtn);
    gui::apply_locus_accessible_name(commit_btn_);
    commit_btn_->SetToolTip(
        "Cancel the current LLM round and tell the model to commit to a "
        "tool call now (or give a brief final answer). Distinct from Stop "
        "(which aborts the whole turn). Surfaces automatically when the "
        "reasoning watchdog detects the model has been thinking past the "
        "configured budget without committing.");
    commit_btn_->Hide();
    commit_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_commit_now_) on_commit_now_();
    });

    undo_btn_ = new wxButton(this, wxID_ANY, "Undo",
                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    undo_btn_->SetName(ui_names::kChatUndoBtn);
    gui::apply_locus_accessible_name(undo_btn_);
    undo_btn_->SetToolTip("Revert files mutated by the most recent turn");
    undo_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_undo_) on_undo_();
    });

    // S5.Z task 2 -- magnifier toggle that opens the find-in-conversation bar.
    // Sits inline with the rest of the chips; Ctrl+F is the other entry point.
    find_btn_ = new wxButton(this, wxID_ANY, "Find",
                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    find_btn_->SetName(ui_names::kChatFindBtn);
    gui::apply_locus_accessible_name(find_btn_);
    find_btn_->SetToolTip("Find in conversation (Ctrl+F)");
    find_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        toggle_find_bar();
    });

    // S5.S -- permission preset dropdown. The static "Permission:" prefix
    // label was retired in the footer declutter pass; the combo carries
    // its own tooltip + accessible name. The combo's foreground colour is
    // updated in on_permission_preset_changed to keep the elevation cue
    // (gray / amber / orange / blue) visible at a glance.
    {
        wxArrayString preset_labels;
        for (auto p : tools::all_presets_in_order())
            preset_labels.Add(tools::display_name(p));
        preset_choice_ = new wxChoice(this, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, preset_labels);
    }
    preset_choice_->SetName(ui_names::kChatPresetChoice);
    gui::apply_locus_accessible_name(preset_choice_);
    preset_choice_->SetToolTip(
        "Permission preset for this tab. Runtime overrides last until the\n"
        "tab is closed -- the persistent setting is in Settings -> Tool Approvals.");
    preset_choice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        int sel = preset_choice_->GetSelection();
        auto order = tools::all_presets_in_order();
        if (sel < 0 || sel >= static_cast<int>(order.size())) return;
        tools::PermissionPreset picked = order[sel];

        // "Custom" from the dropdown is a no-op. Snap back to the effective.
        if (picked == tools::PermissionPreset::custom) {
            on_permission_preset_changed(preset_effective_, preset_is_runtime_);
            return;
        }

        // Confirm modal for elevated presets. Skipped when the pick matches
        // what's already effective so the dropdown can be repainted without
        // re-prompting.
        if (picked != preset_effective_) {
            if (picked == tools::PermissionPreset::allow_edits ||
                picked == tools::PermissionPreset::allow_all)
            {
                wxString title;
                wxString msg;
                if (picked == tools::PermissionPreset::allow_edits) {
                    title = "Apply 'Allow edits' for this tab?";
                    msg = "File edits will run without prompting until you\n"
                          "close this tab. Shell + MCP still ask.\n\n"
                          "The persistent setting under Settings -> Tool\n"
                          "Approvals is not changed.";
                } else {
                    title = "Apply 'Allow all' for this tab?";
                    msg = "File mutations AND shell commands will run\n"
                          "without prompting until you close this tab.\n"
                          "MCP tools still ask -- elevate per server under\n"
                          "Settings -> MCP if needed.\n\n"
                          "Combined with auto-compaction the agent can run\n"
                          "unattended. The persistent setting under Settings\n"
                          "-> Tool Approvals is not changed.";
                }
                wxMessageDialog dlg(this, msg, title,
                                    wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
                dlg.SetYesNoLabels("Apply preset", "Cancel");
                if (dlg.ShowModal() != wxID_YES) {
                    on_permission_preset_changed(preset_effective_,
                                                  preset_is_runtime_);
                    return;
                }
            }
        }

        if (on_permission_preset_pick_) on_permission_preset_pick_(picked);
    });

    refresh_action_btn();
}

// ---------------------------------------------------------------------------
// S5.Z task 2 -- in-conversation find bar
// ---------------------------------------------------------------------------

void ChatPanel::create_find_bar()
{
    find_bar_ = new wxPanel(this, wxID_ANY);
    find_bar_->SetName(ui_names::kChatFindBar);
    gui::apply_locus_accessible_name(find_bar_);

    find_input_ = new wxTextCtrl(find_bar_, wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxDefaultSize,
                                  wxTE_PROCESS_ENTER);
    find_input_->SetName(ui_names::kChatFindInput);
    gui::apply_locus_accessible_name(find_input_);
    find_input_->SetToolTip("Find in conversation. Enter = next, "
                            "Shift+Enter = previous, Esc = close.");
    find_input_->Bind(wxEVT_TEXT, [this](wxCommandEvent& evt) {
        find_apply();
        evt.Skip();
    });
    find_input_->Bind(wxEVT_KEY_DOWN, &ChatPanel::on_find_input_key, this);

    find_counter_ = new wxStaticText(find_bar_, wxID_ANY, "0 / 0");
    find_counter_->SetName(ui_names::kChatFindCounter);
    gui::apply_locus_accessible_name(find_counter_);

    find_prev_btn_ = new wxButton(find_bar_, wxID_ANY, "Prev",
                                   wxDefaultPosition, wxDefaultSize,
                                   wxBU_EXACTFIT);
    find_prev_btn_->SetName(ui_names::kChatFindPrevBtn);
    gui::apply_locus_accessible_name(find_prev_btn_);
    find_prev_btn_->SetToolTip("Previous match (Shift+Enter / Shift+F3)");
    find_prev_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        find_step(/*forward=*/false);
    });

    find_next_btn_ = new wxButton(find_bar_, wxID_ANY, "Next",
                                   wxDefaultPosition, wxDefaultSize,
                                   wxBU_EXACTFIT);
    find_next_btn_->SetName(ui_names::kChatFindNextBtn);
    gui::apply_locus_accessible_name(find_next_btn_);
    find_next_btn_->SetToolTip("Next match (Enter / F3)");
    find_next_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        find_step(/*forward=*/true);
    });

    find_case_toggle_ = new wxCheckBox(find_bar_, wxID_ANY, "Aa");
    find_case_toggle_->SetName(ui_names::kChatFindCaseToggle);
    gui::apply_locus_accessible_name(find_case_toggle_);
    find_case_toggle_->SetToolTip("Match case");
    find_case_toggle_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        find_apply();
    });

    find_close_btn_ = new wxButton(find_bar_, wxID_ANY, "x",
                                    wxDefaultPosition, wxSize(22, 22),
                                    wxBU_EXACTFIT);
    find_close_btn_->SetName(ui_names::kChatFindCloseBtn);
    gui::apply_locus_accessible_name(find_close_btn_);
    find_close_btn_->SetToolTip("Close find bar (Esc)");
    find_close_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        hide_find_bar();
    });

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(find_bar_, wxID_ANY, "Find:"),
             0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);
    row->Add(find_input_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    row->Add(find_counter_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    row->Add(find_prev_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    row->Add(find_next_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    row->Add(find_case_toggle_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    row->Add(find_close_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    find_bar_->SetSizer(row);
    find_bar_->Hide();
}

bool ChatPanel::is_find_bar_visible() const
{
    return find_bar_ && find_bar_->IsShown();
}

void ChatPanel::toggle_find_bar()
{
    if (is_find_bar_visible()) hide_find_bar();
    else                       show_find_bar();
}

void ChatPanel::show_find_bar()
{
    if (!find_bar_) return;
    if (!find_bar_->IsShown()) {
        find_bar_->Show();
        Layout();
    }
    find_input_->SetFocus();
    find_input_->SelectAll();
    // If text is non-empty, ensure the highlight matches the current query.
    if (!find_input_->GetValue().IsEmpty()) find_apply();
}

void ChatPanel::hide_find_bar()
{
    if (!find_bar_) return;
    if (webview_ && !find_active_query_.IsEmpty()) {
        // Cancel any active highlight before tearing down the UI.  wx docs
        // specify Find("") as the explicit cancel path.
        webview_->Find("");
    }
    find_active_query_.clear();
    find_total_ = 0;
    find_index_ = 0;
    update_find_counter();
    if (find_bar_->IsShown()) {
        find_bar_->Hide();
        Layout();
    }
    if (input_) input_->SetFocus();
}

int ChatPanel::current_find_flags() const
{
    int flags = wxWEBVIEW_FIND_WRAP | wxWEBVIEW_FIND_HIGHLIGHT_RESULT;
    if (find_case_toggle_ && find_case_toggle_->GetValue())
        flags |= wxWEBVIEW_FIND_MATCH_CASE;
    return flags;
}

void ChatPanel::find_apply()
{
    if (!webview_ || !find_input_) return;

    wxString query = find_input_->GetValue();

    // Empty input: cancel any active highlight and clear the counter.
    if (query.IsEmpty()) {
        if (!find_active_query_.IsEmpty()) webview_->Find("");
        find_active_query_.clear();
        find_total_ = 0;
        find_index_ = 0;
        update_find_counter();
        return;
    }

    // The wx contract: a fresh Find() with a NEW phrase returns the total
    // match count; subsequent same-phrase calls advance through results.
    // To get a fresh count after a case-toggle or query change we must clear
    // first.
    if (!find_active_query_.IsEmpty()) webview_->Find("");

    long total = webview_->Find(query, current_find_flags());
    find_active_query_ = query;
    if (total <= 0) {
        find_total_ = 0;
        find_index_ = 0;
    } else {
        find_total_ = total;
        find_index_ = 1;  // wx scrolls + selects the first match.
    }
    update_find_counter();
}

void ChatPanel::find_step(bool forward)
{
    if (!webview_) return;

    // No active query yet: treat as initial apply.
    if (find_active_query_.IsEmpty()) {
        find_apply();
        return;
    }
    if (find_total_ <= 0) {
        update_find_counter();
        return;
    }

    int flags = current_find_flags();
    if (!forward) flags |= wxWEBVIEW_FIND_BACKWARDS;
    webview_->Find(find_active_query_, flags);

    if (forward) {
        find_index_ = (find_index_ % find_total_) + 1;
    } else {
        find_index_ = (find_index_ <= 1) ? find_total_ : (find_index_ - 1);
    }
    update_find_counter();
}

void ChatPanel::update_find_counter()
{
    if (!find_counter_) return;
    find_counter_->SetLabel(
        wxString::Format("%ld / %ld", find_index_, find_total_));
    if (find_bar_) find_bar_->Layout();
}

void ChatPanel::on_find_input_key(wxKeyEvent& evt)
{
    const int key = evt.GetKeyCode();
    if (key == WXK_ESCAPE) {
        hide_find_bar();
        return;
    }
    if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
        find_step(/*forward=*/!evt.ShiftDown());
        return;
    }
    evt.Skip();
}

// ---------------------------------------------------------------------------
// Public API -- called from LocusFrame event handlers
// ---------------------------------------------------------------------------

void ChatPanel::on_turn_start()
{
    renderer_->begin_turn();
    refresh_action_btn();
    if (undo_btn_) undo_btn_->Disable();
    flush_timer_.Start(k_flush_interval_ms);
}

void ChatPanel::on_token(const wxString& token)
{
    renderer_->append_token(token);
}

void ChatPanel::on_reasoning_token(const wxString& token)
{
    renderer_->append_reasoning_token(token);
}

void ChatPanel::on_turn_complete()
{
    flush_timer_.Stop();
    renderer_->end_turn();

    // S5.D -- annotate the assistant bubble with server-reported completion
    // tokens once the turn is sealed (end_turn has finalized the HTML).
    // Append a tokens-per-second figure when the last LLM round was long
    // enough to make the rate meaningful (format_tok_per_sec applies the
    // floor); short replies or instant cached responses drop the rate.
    if (show_per_message_tokens_ && last_completion_tokens_ > 0) {
        int aid = renderer_->current_assistant_id();
        if (aid > 0) {
            auto rate = format_tok_per_sec(last_completion_tokens_, last_stream_ms_);
            if (rate)
                run_script(wxString::Format("setMsgTokenChip(%d, %d, '%s');",
                                            aid, last_completion_tokens_,
                                            chat_js_escape(*rate)));
            else
                run_script(wxString::Format("setMsgTokenChip(%d, %d);",
                                            aid, last_completion_tokens_));
        }
    }
    // Drop the round-scoped sample so the NEXT turn starts clean -- without
    // this, a turn whose final round was untracked (e.g. cancelled) would
    // pick up the previous turn's rate.
    last_stream_ms_ = 0;

    footer_chips_->clear_live_estimate();
    if (footer_chips_->hide_round_progress()) Layout();
    refresh_action_btn();
    if (undo_btn_) undo_btn_->Enable();
    input_->SetFocus();
}

void ChatPanel::on_session_reset()
{
    run_script("clearChat();");
    renderer_->reset();
    link_handler_->clear_plans();
    if (footer_chips_->hide_plan_chip()) Layout();
    on_mode_changed(AgentMode::chat);
    refresh_action_btn();
    message_id_ = 0;
    tool_call_msg_ids_.clear();
    pending_tool_info_.clear();
    // S5.G -- reset chat-side delete bookkeeping.
    history_to_dom_.clear();
    pending_user_dom_ids_.clear();
    pending_tool_history_id_ = 0;
    // S5.Z task 2 -- previous find state references DOM that no longer exists.
    if (is_find_bar_visible()) hide_find_bar();
}

// ---------------------------------------------------------------------------
// S4.D plan-mode display
// ---------------------------------------------------------------------------

void ChatPanel::on_mode_changed(AgentMode mode)
{
    if (mode_chat_btn_)    mode_chat_btn_->SetValue(mode == AgentMode::chat);
    if (mode_plan_btn_)    mode_plan_btn_->SetValue(mode == AgentMode::plan);
    if (mode_execute_btn_) mode_execute_btn_->SetValue(mode == AgentMode::execute);

    if (mode == AgentMode::chat) {
        if (footer_chips_->hide_plan_chip()) Layout();
    }
}

void ChatPanel::on_plan_proposed(const wxString& plan_json)
{
    int total = 0;
    std::string id;
    std::string first_step;
    try {
        auto j = nlohmann::json::parse(plan_json.utf8_string());
        id = j.value("id", "");
        if (j.contains("steps") && j["steps"].is_array()) {
            total = static_cast<int>(j["steps"].size());
            if (total > 0)
                first_step = j["steps"][0].value("description", "");
        }
    } catch (const nlohmann::json::exception&) {
        spdlog::warn("ChatPanel: malformed plan JSON; rendering raw");
    }

    ++message_id_;
    link_handler_->register_plan(id, message_id_);
    if (footer_chips_->on_plan_proposed(id, total, first_step)) Layout();

    run_script(wxString::Format("addPlanMsg(%d, '%s');",
                                message_id_, chat_js_escape(plan_json)));
}

void ChatPanel::on_plan_step_advanced(const wxString& plan_id, int step_idx,
                                       const wxString& status,
                                       const wxString& notes)
{
    const std::string id_str(plan_id.utf8_string());
    int msg_id = link_handler_->lookup_msg_id(id_str);
    if (msg_id < 0) {
        spdlog::warn("ChatPanel: step advance for unknown plan id '{}'",
                     id_str);
        return;
    }

    run_script(wxString::Format(
        "updatePlanStep(%d, %d, '%s', '%s');",
        msg_id, step_idx, chat_js_escape(status), chat_js_escape(notes)));

    if (footer_chips_->on_plan_step_advanced(id_str, status)) Layout();
}

void ChatPanel::on_plan_completed(const wxString& plan_id, bool success)
{
    const std::string id_str(plan_id.utf8_string());
    int msg_id = link_handler_->lookup_msg_id(id_str);
    if (msg_id >= 0) {
        wxString label = success ? "Plan completed."
                                  : "Plan completed with failures.";
        run_script(wxString::Format(
            "setPlanDecided(%d, '%s');",
            msg_id, chat_js_escape(label)));
    }
    if (footer_chips_->on_plan_completed(id_str, success)) Layout();
}

void ChatPanel::on_error(const wxString& message)
{
    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-error', %s);",
        message_id_, "'" + chat_js_escape(message) + "'"));

    // S5.Z follow-up -- agentic Tetris testing exposed that on_error left
    // the chat panel pinned in "streaming" state: the renderer's streaming_
    // flag stayed true, the flush timer kept firing, and the Stop button
    // never reverted to Submit. The agentic harness watches stop_btn_visible
    // (and humans hit the same wall on a hung agent), so an error must
    // unwind the per-turn state symmetrically with on_turn_complete.
    //
    // We don't reuse on_turn_complete because it also writes the per-message
    // token chip and clears the live-token estimate as if the turn succeeded;
    // both would be misleading on the error path.
    if (flush_timer_.IsRunning()) flush_timer_.Stop();
    if (renderer_ && renderer_->is_streaming())
        renderer_->end_turn();
    footer_chips_->clear_live_estimate();
    if (footer_chips_->hide_round_progress()) Layout();
    refresh_action_btn();
    if (undo_btn_) undo_btn_->Enable();
}

void ChatPanel::on_auto_commit(const wxString& short_sha,
                               const wxString& branch,
                               const wxString& subject)
{
    if (footer_chips_->on_auto_commit(short_sha, branch, subject)) Layout();
}

wxString ChatPanel::plan_status_text() const
{
    return footer_chips_ ? footer_chips_->plan_text() : wxString{};
}

wxString ChatPanel::commit_status_text() const
{
    return footer_chips_ ? footer_chips_->commit_text() : wxString{};
}

void ChatPanel::set_compacted_count(int count, const wxString& archive_dir)
{
    if (footer_chips_->set_compacted_count(count, archive_dir)) Layout();
}

void ChatPanel::set_round_progress(int round, int max_rounds)
{
    if (footer_chips_->set_round_progress(round, max_rounds)) Layout();
}

void ChatPanel::set_commit_now_visible(bool visible)
{
    if (!commit_btn_) return;
    if (commit_btn_->IsShown() == visible) return;  // idempotent / no Layout churn
    commit_btn_->Show(visible);
    Layout();
}

void ChatPanel::on_tool_pending(const wxString& call_id,
                                const wxString& tool_name,
                                const wxString& preview,
                                const nlohmann::json& args)
{
    // Cache args + tool name for diff rendering in on_tool_result.
    {
        PendingToolInfo info;
        info.tool_name = std::string(tool_name.utf8_string());
        info.args      = args;
        pending_tool_info_[std::string(call_id.utf8_string())] = std::move(info);
    }

    // S5.Z #1 -- seal the current streaming bubble before inserting the tool bubble.
    renderer_->seal_bubble();
    // seal_bubble clears waiting_for_first_token_ internally.

    ++message_id_;
    tool_call_msg_ids_[std::string(call_id.utf8_string())] = message_id_;

    wxString content = "<span class=\"tool-name\">" + chat_js_escape(tool_name) + "</span>";
    if (!preview.empty())
        content += "<br><span class=\"tool-preview\">" + chat_js_escape(preview) + "</span>";

    run_script(wxString::Format(
        "addMsg(%d, 'msg-tool', %s);",
        message_id_, "'" + chat_js_escape(content) + "'"));
}

void ChatPanel::on_tool_result(const wxString& call_id,
                               const wxString& display,
                               bool success)
{
    const std::string call_id_str(call_id.utf8_string());

    std::optional<PendingToolInfo> pending;
    if (auto it = pending_tool_info_.find(call_id_str);
        it != pending_tool_info_.end()) {
        pending = std::move(it->second);
        pending_tool_info_.erase(it);
    }

    if (display.empty() && !success) return;
    if (display.empty() && !pending.has_value()) return;

    int target_id = message_id_;
    auto it = tool_call_msg_ids_.find(call_id_str);
    if (it != tool_call_msg_ids_.end()) {
        target_id = it->second;
        tool_call_msg_ids_.erase(it);
    }

    // Pair the tool message's history_id (parked by on_history_message_added
    // which fires immediately before this) with its dom bubble id. Used for
    // two paths: (a) pair-delete from the parent assistant cascades to this
    // tool row via history_to_dom_; (b) the hover-reveal X on the tool bubble
    // itself routes a delete-message request whose target is the tool's
    // history_id -- LLMContext::delete_message walks back to the parent
    // assistant and pair-deletes the whole turn.
    if (pending_tool_history_id_ > 0) {
        history_to_dom_[pending_tool_history_id_] = target_id;
        run_script(wxString::Format("addDeleteButton(%d, %d);",
                                    target_id, pending_tool_history_id_));
        pending_tool_history_id_ = 0;
    }

    // S5.C -- inline diff for successful edit_file / write_file / delete_file.
    if (success && diff_show_ && pending.has_value()) {
        const std::string& tool_name = pending->tool_name;
        const auto&        args      = pending->args;

        std::string diff_html;
        DiffRenderOptions opts;
        opts.max_lines          = diff_max_lines_;
        opts.context_lines      = diff_context_lines_;
        opts.collapse_threshold = diff_collapse_threshold_;

        if (tool_name == "edit_file") {
            // edit_file's canonical path arg is `file_path` (renamed from
            // `path` for small-model robustness); accept both for diff
            // rendering so saved sessions + legacy calls still draw.
            std::string path = args.value("file_path", std::string{});
            if (path.empty()) path = args.value("path", std::string{});
            std::optional<std::string> old_content;
            if (pre_mutation_fetcher_ && !path.empty())
                old_content = pre_mutation_fetcher_(path);
            diff_html = render_edit_file_diff_html(args, old_content, opts);
        } else if (tool_name == "write_file") {
            const std::string path        = args.value("path", std::string{});
            std::string       new_content = args.value("content", std::string{});
            std::optional<std::string> old_content;
            if (pre_mutation_fetcher_ && !path.empty())
                old_content = pre_mutation_fetcher_(path);
            diff_html = render_write_file_diff_html(path, old_content,
                                                    new_content, opts);
        } else if (tool_name == "delete_file") {
            const std::string path = args.value("path", std::string{});
            int line_count = 0;
            if (pre_mutation_fetcher_ && !path.empty()) {
                auto old_content = pre_mutation_fetcher_(path);
                if (old_content.has_value()) {
                    for (char c : *old_content) if (c == '\n') ++line_count;
                    if (!old_content->empty() && old_content->back() != '\n')
                        ++line_count;
                }
            }
            diff_html = render_delete_file_summary_html(path, line_count);
        }

        if (!diff_html.empty()) {
            run_script(wxString::Format(
                "var d=document.getElementById('msg-%d');"
                "if(d){var w=document.createElement('div');"
                "w.className='tool-diff-wrap';"
                "w.innerHTML=%s;"
                "d.appendChild(w);"
                "window.scrollTo(0,document.body.scrollHeight);}",
                target_id,
                "'" + chat_js_escape(wxString::FromUTF8(diff_html)) + "'"));
            if (tool_name == "edit_file"  ||
                tool_name == "write_file" ||
                tool_name == "delete_file") {
                return;
            }
        }
    }

    if (display.empty()) return;

    wxString truncated = display;
    if (truncated.length() > 500)
        truncated = truncated.Left(500) + "... (" +
                    wxString::Format("%zu", display.length() - 500) + " chars truncated)";

    run_script(wxString::Format(
        "var d=document.getElementById('msg-%d');"
        "if(d){var det=document.createElement('details');"
        "det.className='tool-result-details%s';"
        "var sum=document.createElement('summary');"
        "sum.textContent=%s;"
        "det.appendChild(sum);"
        "var pre=document.createElement('pre');"
        "pre.className='tool-result';pre.textContent=%s;"
        "det.appendChild(pre);"
        "d.appendChild(det);"
        "window.scrollTo(0,document.body.scrollHeight);}",
        target_id,
        success ? "" : " tool-result-error",
        success ? "'Result'" : "'Error'",
        "'" + chat_js_escape(truncated) + "'"));

    // S5.G -- token chip on tool bubbles too. Estimate from the display text
    // (the bulk of the tool message's content) + framing -- close enough to
    // the ConversationHistory.tool_result message's token_estimate for the
    // chat panel's purposes.
    if (show_per_message_tokens_ && !display.empty()) {
        int est = TokenCounter::estimate(display.ToStdString(wxConvUTF8)) + 4;
        run_script(wxString::Format("setMsgTokenChip(%d, %d);", target_id, est));
    }
}

void ChatPanel::set_context_meter(int used, int limit,
                                   int prompt_tokens, int completion_tokens,
                                   int reserve_tokens,
                                   long long stream_ms_last_round)
{
    if (completion_tokens > 0) last_completion_tokens_ = completion_tokens;
    // stream_ms is only non-zero on the broadcast that fires immediately
    // after an LLM round; the session-open, compaction, reset, and
    // mid-turn meter broadcasts all pass 0 and should NOT clobber the
    // value the chat panel needs at on_turn_complete time.
    if (stream_ms_last_round > 0) last_stream_ms_ = stream_ms_last_round;
    footer_chips_->set_context_meter(used, limit, prompt_tokens,
                                      completion_tokens, reserve_tokens);
}

void ChatPanel::set_show_per_message_tokens(bool show)
{
    show_per_message_tokens_ = show;
}

void ChatPanel::set_auto_compact_state(bool checked)
{
    if (auto_compact_cb_) auto_compact_cb_->SetValue(checked);
}

// ---------------------------------------------------------------------------
// S5.S -- permission preset chip
// ---------------------------------------------------------------------------

void ChatPanel::on_permission_preset_changed(tools::PermissionPreset effective,
                                             bool is_runtime_override)
{
    preset_effective_  = effective;
    preset_is_runtime_ = is_runtime_override;

    if (preset_choice_) {
        auto order = tools::all_presets_in_order();
        int idx = 0;
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i] == effective) { idx = static_cast<int>(i); break; }
        }
        preset_choice_->SetSelection(idx);

        // Elevation cue moves from the retired "Permission:" chip onto the
        // combo's foreground colour. Native wxChoice on Windows respects
        // SetForegroundColour for the closed-state selected-item text.
        wxColour fg = wxColour(80, 80, 80);
        switch (effective) {
        case tools::PermissionPreset::read_only:
        case tools::PermissionPreset::ask_before_edits:
            fg = wxColour(80, 80, 80);     break;
        case tools::PermissionPreset::allow_edits:
            fg = wxColour(178, 130, 0);    break;
        case tools::PermissionPreset::allow_all:
            fg = wxColour(200, 90, 0);     break;
        case tools::PermissionPreset::custom:
            fg = wxColour(60, 90, 180);    break;
        }
        preset_choice_->SetForegroundColour(fg);

        wxString tip;
        switch (effective) {
        case tools::PermissionPreset::read_only:
            tip = "Read-only: mutations denied";          break;
        case tools::PermissionPreset::ask_before_edits:
            tip = "Asks before every mutation";           break;
        case tools::PermissionPreset::allow_edits:
            tip = "Files auto-approved; shell + delete + MCP ask"; break;
        case tools::PermissionPreset::allow_all:
            tip = "Files + shell auto-approved; MCP still asks";   break;
        case tools::PermissionPreset::custom:
            tip = "Custom per-tool overrides";            break;
        }
        if (is_runtime_override)
            tip += " (runtime override -- closes with this tab)";
        else
            tip += " (from workspace setting)";
        preset_choice_->SetToolTip(tip);
        preset_choice_->Refresh();
    }
    Layout();
}

// ---------------------------------------------------------------------------
// S5.G -- system-prompt bubble + per-message delete + add/delete hooks
// ---------------------------------------------------------------------------

void ChatPanel::set_system_prompt_bubble(const SystemPromptAssembly& assembly)
{
    // dom_id 0 is reserved for the system-prompt bubble. It lives outside the
    // monotonic message_id_ counter so a session reset (which zeros
    // message_id_) won't collide with this slot.
    nlohmann::json breakdown;
    breakdown["total"] = assembly.total_tokens();
    nlohmann::json sections = nlohmann::json::array();
    auto push_section = [&](const char* label, int tokens) {
        if (tokens <= 0) return;
        nlohmann::json s;
        s["label"]  = label;
        s["tokens"] = tokens;
        sections.push_back(std::move(s));
    };
    push_section("Base",      assembly.base_tokens());
    push_section("Metadata",  assembly.metadata_tokens());
    push_section("LOCUS.md",  assembly.locus_md_tokens());
    push_section("Memory",    assembly.memory_tokens());
    push_section("Tools",     assembly.manifest_tokens());
    push_section("Format",    assembly.format_addendum_tokens());
    breakdown["sections"] = std::move(sections);

    run_script(wxString::Format(
        "addSystemPromptMsg(0, %s, %s);",
        "'" + chat_js_escape(wxString::FromUTF8(assembly.full_text())) + "'",
        chat_js_escape(wxString::FromUTF8(breakdown.dump()))));
}

void ChatPanel::on_history_message_added(int history_id, MessageRole role,
                                          bool deletable)
{
    if (history_id <= 0) return;
    // Tool messages: park the history_id so on_tool_result can pair it with
    // the dom_id created in on_tool_call_pending.  Pair-delete needs the
    // tool history_id -> dom_id mapping so the dom rows actually disappear
    // when the assistant pair-delete is triggered.
    if (role == MessageRole::tool) {
        pending_tool_history_id_ = history_id;
        return;
    }
    int target_dom_id = 0;
    if (role == MessageRole::user) {
        // FIFO: each submit pushes a dom_id; the next role=user
        // on_history_message_added pops the front. With mid-turn injection
        // (`AgentCore` may pair multiple in-flight user bubbles back-to-back
        // against multiple queued submits), the deque keeps pairings stable
        // even when 2+ submits hit before the first one is injected.
        if (!pending_user_dom_ids_.empty()) {
            target_dom_id = pending_user_dom_ids_.front();
            pending_user_dom_ids_.pop_front();
        }
    } else if (role == MessageRole::assistant) {
        // Live path: the agent loop commits the assistant message to history
        // AFTER the stream renderer has sealed the bubble and zeroed its
        // assistant_id_. Fall back to the last-sealed dom_id so the
        // hover-reveal X attaches on the same run instead of only after a
        // session reload. (S5.G follow-up; pre-fix the X was visible only
        // after close + reopen.)
        if (renderer_) {
            target_dom_id = renderer_->current_assistant_id();
            if (target_dom_id <= 0)
                target_dom_id = renderer_->last_sealed_assistant_id();
        }
    }
    // system role isn't user-deletable; skip mapping.

    if (target_dom_id <= 0 || !deletable) return;

    history_to_dom_[history_id] = target_dom_id;
    run_script(wxString::Format("addDeleteButton(%d, %d);",
                                target_dom_id, history_id));
}

void ChatPanel::on_history_message_deleted(int history_id)
{
    if (history_id <= 0) return;
    auto it = history_to_dom_.find(history_id);
    if (it == history_to_dom_.end()) return;
    int dom_id = it->second;
    history_to_dom_.erase(it);
    run_script(wxString::Format("removeMsg(%d);", dom_id));
}

void ChatPanel::render_loaded_history(const ConversationHistory& history,
                                      IToolRegistry* tools)
{
    // Per-call lookup so tool-result bubbles below can recover the tool name
    // AND re-build the same `tool-preview` line the live `on_tool_pending`
    // path renders. Walk forward so each tool result only needs to consult
    // already-seen assistant turns.
    struct CallInfo {
        std::string tool_name;
        std::string preview;   // empty when no registry or tool returned ""
    };
    std::unordered_map<std::string, CallInfo> call_info;

    auto build_preview = [&](const std::string& tool_name,
                             const std::string& args_json) -> std::string {
        if (!tools) return {};
        ITool* t = tools->find(tool_name);
        if (!t) return {};
        ToolCall tc;
        tc.tool_name = tool_name;
        if (!args_json.empty()) {
            try { tc.args = nlohmann::json::parse(args_json); }
            catch (...) { tc.args = nlohmann::json::object(); }
        }
        try { return t->preview(tc); }
        catch (...) { return {}; }
    };

    for (const auto& msg : history.messages()) {
        switch (msg.role) {
        case MessageRole::system:
            // Rendered separately as the collapsed system_prompt bubble.
            continue;

        case MessageRole::user: {
            ++message_id_;
            wxString html = user_text_to_html(wxString::FromUTF8(msg.content));
            run_script(wxString::Format(
                "addMsg(%d, 'msg-user', %s);",
                message_id_, "'" + chat_js_escape(html) + "'"));
            // Saved-session render path always knows history_id, so the
            // pending-pairing queue stays untouched here -- direct
            // history_to_dom_ wiring below handles the X-attach.
            if (msg.history_id > 0) {
                history_to_dom_[msg.history_id] = message_id_;
                run_script(wxString::Format("addDeleteButton(%d, %d);",
                                            message_id_, msg.history_id));
            }
            if (show_per_message_tokens_ && msg.token_estimate > 0) {
                run_script(wxString::Format(
                    "setMsgTokenChip(%d, %d);", message_id_, msg.token_estimate));
            }
            break;
        }

        case MessageRole::assistant: {
            // Cache args+preview for each tool_call so the matching tool-
            // result bubble below can label itself + show the same
            // `tool-preview` line live mode shows.
            for (const auto& tc : msg.tool_calls) {
                if (tc.id.empty()) continue;
                CallInfo info;
                info.tool_name = tc.name;
                info.preview   = build_preview(tc.name, tc.arguments);
                call_info[tc.id] = std::move(info);
            }

            // Restore the collapsed `Thinking...` bubble whenever the saved
            // assistant turn carried reasoning_content (S6.10 Task C persists
            // it). Live mode emits this via addReasoning + setReasoningBody +
            // finalizeReasoning; we replay the same three calls so the
            // resulting DOM matches byte-for-byte.
            if (!msg.reasoning_content.empty()) {
                ++message_id_;
                int rid = message_id_;
                run_script(wxString::Format("addReasoning(%d, 0);", rid));
                run_script(wxString::Format(
                    "setReasoningBody(%d, %s);",
                    rid,
                    "'" + chat_js_escape(wxString::FromUTF8(msg.reasoning_content)) + "'"));
                run_script(wxString::Format(
                    "finalizeReasoning(%d, 'Thinking (restored)');", rid));
            }

            if (msg.content.empty()) {
                // tool-only assistant turn -- no visible content bubble
                // (matches the live experience where the assistant bubble
                // seals empty and gets replaced by the tool bubble). The
                // tool-preview line below still appears above each result.
                continue;
            }
            ++message_id_;
            std::string body = markdown_to_html(msg.content);
            run_script(wxString::Format(
                "addMsg(%d, 'msg-assistant', %s);",
                message_id_,
                "'" + chat_js_escape(wxString::FromUTF8(body)) + "'"));
            // Assistants with content are deletable; the X on an assistant
            // that has tool_calls triggers a pair-delete inside LLMContext.
            if (msg.history_id > 0) {
                history_to_dom_[msg.history_id] = message_id_;
                run_script(wxString::Format("addDeleteButton(%d, %d);",
                                            message_id_, msg.history_id));
            }
            if (show_per_message_tokens_ && msg.token_estimate > 0) {
                run_script(wxString::Format(
                    "setMsgTokenChip(%d, %d);", message_id_, msg.token_estimate));
            }
            break;
        }

        case MessageRole::tool: {
            ++message_id_;
            std::string tool_name = "tool";
            std::string preview;
            if (auto it = call_info.find(msg.tool_call_id);
                it != call_info.end()) {
                tool_name = it->second.tool_name;
                preview   = it->second.preview;
            }
            // Bubble head: tool-name + optional preview line. Matches live
            // `on_tool_pending` exactly so the restored DOM is identical.
            // The result body is appended below via DOM construction so its
            // textContent goes through exactly one JS-string decode --
            // building inline HTML would force a second chat_js_escape pass
            // on already-escaped content, turning newlines into literal `\n`.
            wxString head = "<span class=\"tool-name\">"
                          + chat_js_escape(wxString::FromUTF8(tool_name))
                          + "</span>";
            if (!preview.empty()) {
                head += "<br><span class=\"tool-preview\">"
                      + chat_js_escape(wxString::FromUTF8(preview))
                      + "</span>";
            }
            run_script(wxString::Format(
                "addMsg(%d, 'msg-tool', %s);",
                message_id_,
                "'" + chat_js_escape(head) + "'"));
            run_script(wxString::Format(
                "var d=document.getElementById('msg-%d');"
                "if(d){var det=document.createElement('details');"
                "det.className='tool-result-details';"
                "var sum=document.createElement('summary');"
                "sum.textContent='Result';"
                "det.appendChild(sum);"
                "var pre=document.createElement('pre');"
                "pre.className='tool-result';pre.textContent=%s;"
                "det.appendChild(pre);"
                "d.appendChild(det);}",
                message_id_,
                "'" + chat_js_escape(wxString::FromUTF8(msg.content)) + "'"));
            // Map the tool history_id -> dom_id and attach the hover-reveal
            // delete X. Clicking it routes through LLMContext::delete_message
            // which walks back to the parent assistant and pair-deletes the
            // whole turn (matches the live on_tool_result wiring above).
            if (msg.history_id > 0) {
                history_to_dom_[msg.history_id] = message_id_;
                run_script(wxString::Format("addDeleteButton(%d, %d);",
                                            message_id_, msg.history_id));
            }
            if (show_per_message_tokens_ && msg.token_estimate > 0) {
                run_script(wxString::Format(
                    "setMsgTokenChip(%d, %d);", message_id_, msg.token_estimate));
            }
            break;
        }
        }
    }
    // Final highlight pass so Prism colors every <pre><code> at once.
    run_script("highlightAll();");
}

void ChatPanel::set_generation_progress(int chars, int est_tokens)
{
    footer_chips_->set_generation_progress(chars, est_tokens);
}

void ChatPanel::set_attached_chip(const wxString& file_path)
{
    if (file_path.empty()) {
        if (attach_panel_->IsShown()) {
            attach_panel_->Hide();
            Layout();
        }
        attach_label_->SetLabel("");
        return;
    }
    attach_label_->SetLabel(wxString("Attached: ") + file_path);
    attach_label_->SetToolTip(file_path);
    if (!attach_panel_->IsShown()) {
        attach_panel_->Show();
        Layout();
    }
}

void ChatPanel::set_on_detach(std::function<void()> cb)
{
    on_detach_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Timer: flush tokens to WebView
// ---------------------------------------------------------------------------

void ChatPanel::on_flush_timer(wxTimerEvent& /*evt*/)
{
    renderer_->flush();
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

void ChatPanel::on_input_key(wxKeyEvent& evt)
{
    // Route nav keys to whichever popup is visible first.
    if (popups_->handle_key(evt)) return;

    const int key = evt.GetKeyCode();
    if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && !evt.ShiftDown()) {
        submit_current_input();
    } else {
        evt.Skip();
    }
}

void ChatPanel::on_input_text(wxCommandEvent& evt)
{
    popups_->update();
    refresh_action_btn();
    evt.Skip();
}

bool ChatPanel::submit_current_input()
{
    wxString text = input_->GetValue().Trim().Trim(false);
    if (text.empty()) return false;

    // GUI slash dispatch.
    if (on_slash_command_ && !text.empty() && text[0] == '/') {
        wxString body = text.Mid(1);
        auto is_ws = [](wxUniChar c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        };
        size_t sp = 0;
        while (sp < body.size() && !is_ws(body[sp])) ++sp;
        std::string name = body.Left(sp).ToStdString();
        std::string rest;
        if (sp < body.size()) {
            wxString r = body.Mid(sp);
            r.Trim(false);
            rest = r.ToStdString();
        }
        if (!name.empty() && on_slash_command_(name, rest)) {
            input_->Clear();
            popups_->dismiss_all();
            refresh_action_btn();
            return true;
        }
    }

    input_->Clear();
    popups_->dismiss_all();
    refresh_action_btn();

    // S4.V -- extract first @<path> mention and auto-attach.
    if (on_mention_attach_ && !popups_->mention_paths().empty()) {
        auto std_text = text.ToStdString(wxConvUTF8);
        auto mentions = parse_mentions(std_text);
        std::unordered_set<std::string> known(
            popups_->mention_paths().begin(), popups_->mention_paths().end());
        for (const auto& m : mentions) {
            if (known.find(m.path) != known.end()) {
                on_mention_attach_(m.path);
                break;
            }
        }
    }

    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-user', %s);",
        message_id_, "'" + chat_js_escape(user_text_to_html(text)) + "'"));

    // S5.G -- remember this dom_id so the matching on_history_message_added
    // (which fires once the agent thread has appended the user ChatMessage,
    // either at turn start OR via mid-turn round-boundary injection) can
    // attach a delete X to the right bubble. FIFO queue handles multiple
    // submits queued back-to-back while the agent is mid-turn.
    pending_user_dom_ids_.push_back(message_id_);

    // S5.D -- per-message token chip: client-side heuristic estimate on the
    // raw user text (effective_content may be slightly larger due to attached-
    // context and file-change prefixes; this is close enough for the UI chip).
    if (show_per_message_tokens_) {
        int est = TokenCounter::estimate(text.ToStdString(wxConvUTF8)) + 4;
        run_script(wxString::Format("setMsgTokenChip(%d, %d);", message_id_, est));
    }

    if (on_send_)
        on_send_(text.ToStdString(wxConvUTF8));
    return true;
}

// -- Slash / mention delegation --

void ChatPanel::set_slash_commands(std::vector<SlashItem> items)
{
    popups_->set_slash_commands(std::move(items));
}

void ChatPanel::set_on_slash_command(
    std::function<bool(const std::string&, const std::string&)> cb)
{
    on_slash_command_ = std::move(cb);
}

void ChatPanel::append_system_note(const wxString& html)
{
    ++message_id_;
    run_script(wxString::Format(
        "addMsg(%d, 'msg-tool', %s);",
        message_id_, "'" + chat_js_escape(html) + "'"));
}

void ChatPanel::set_mention_paths(std::vector<std::string> paths)
{
    popups_->set_mention_paths(std::move(paths));
}

void ChatPanel::set_on_mention_attach(std::function<void(const std::string&)> cb)
{
    on_mention_attach_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Action button
// ---------------------------------------------------------------------------

void ChatPanel::refresh_action_btn()
{
    if (!stop_btn_) return;
    const bool has_text = input_ &&
        !input_->GetValue().Trim().Trim(false).IsEmpty();
    const bool streaming = renderer_ && renderer_->is_streaming();

    if (streaming) {
        if (has_text) {
            stop_btn_->SetLabel("Queue");
            stop_btn_->SetToolTip(
                "Queue this message. The agent will pick it up after the "
                "current turn completes.");
        } else {
            stop_btn_->SetLabel("Stop");
            stop_btn_->SetToolTip("Stop the current generation");
        }
        stop_btn_->Enable();
    } else {
        stop_btn_->SetLabel("Submit");
        stop_btn_->SetToolTip("Submit the message (Enter)");
        stop_btn_->Enable(has_text);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void ChatPanel::run_script(const wxString& js)
{
    if (!webview_) return;
    if (!page_ready_ || in_run_script_) {
        pending_scripts_.push_back(js);
        return;
    }

    in_run_script_ = true;
    webview_->RunScript(js);
    while (page_ready_ && !pending_scripts_.empty()) {
        wxString next = pending_scripts_.front();
        pending_scripts_.erase(pending_scripts_.begin());
        webview_->RunScript(next);
    }
    in_run_script_ = false;
}

wxString ChatPanel::user_text_to_html(const wxString& s)
{
    wxString norm = s;
    norm.Replace("\r\n", "\n", true);
    norm.Replace("\r",   "\n", true);
    norm.Replace("\v", "\n", true);
    norm.Replace("\f", "\n", true);
    norm.Replace(wxString::FromUTF8("\xC2\x85"),     "\n", true);
    norm.Replace(wxString::FromUTF8("\xE2\x80\xA8"), "\n", true);
    norm.Replace(wxString::FromUTF8("\xE2\x80\xA9"), "\n", true);

    wxString out;
    out.reserve(norm.length() + 16);
    for (auto ch : norm) {
        switch (ch.GetValue()) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\n': out += "<br>";   break;
        default:   out += ch;       break;
        }
    }
    return out;
}

void ChatPanel::on_webview_navigating(wxWebViewEvent& evt)
{
    wxString url = evt.GetURL();

    if (!page_ready_) return;
    if (url.StartsWith("javascript:")) return;

    if (link_handler_->handle_url(url, footer_chips_->current_plan_id())) {
        evt.Veto();
        return;
    }

    spdlog::trace("WebView navigation blocked: {}", url.ToStdString());
    evt.Veto();
}

} // namespace locus
