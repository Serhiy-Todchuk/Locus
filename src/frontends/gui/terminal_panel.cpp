#include "terminal_panel.h"
#include "locus_accessible.h"
#include "theme.h"
#include "ui_names.h"

#include <spdlog/spdlog.h>

#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/clipbrd.h>

#include <algorithm>
#include <deque>
#include <unordered_map>

namespace locus {

namespace {

// 16-entry default palette. Approximations of the classic VT100 colours,
// tuned a bit lighter so they read on a near-white background. wxColour's
// ctor isn't constexpr, so we keep this as a function-local static initialised
// at first use.
const wxColour& palette(std::size_t idx)
{
    static const wxColour entries[16] = {
        wxColour(  0,   0,   0),   // 0  black
        wxColour(178,  24,  44),   // 1  red
        wxColour( 18, 154,  72),   // 2  green
        wxColour(170, 119,  10),   // 3  yellow (orange-ish for readability)
        wxColour( 30, 102, 209),   // 4  blue
        wxColour(170,  60, 178),   // 5  magenta
        wxColour( 20, 152, 168),   // 6  cyan
        wxColour(180, 180, 180),   // 7  white
        wxColour( 80,  80,  80),   // 8  bright black
        wxColour(230,  60,  80),   // 9  bright red
        wxColour( 36, 194, 100),   // 10 bright green
        wxColour(216, 160,  30),   // 11 bright yellow
        wxColour( 70, 140, 240),   // 12 bright blue
        wxColour(210,  98, 220),   // 13 bright magenta
        wxColour( 50, 196, 212),   // 14 bright cyan
        wxColour(230, 230, 230),   // 15 bright white
    };
    return entries[idx];
}

constexpr int k_style_default = 0;

} // namespace

wxBEGIN_EVENT_TABLE(TerminalPanel, wxPanel)
    EVT_TIMER(wxID_ANY, TerminalPanel::on_flush_timer)
    EVT_MENU(ID_TERM_COPY_ALL, TerminalPanel::on_context_menu_action)
    EVT_MENU(ID_TERM_COPY_SEL, TerminalPanel::on_context_menu_action)
    EVT_MENU(ID_TERM_CLEAR,    TerminalPanel::on_context_menu_action)
    EVT_MENU(ID_TERM_KILL,     TerminalPanel::on_context_menu_action)
    EVT_TEXT_ENTER(ID_TERM_STDIN_INPUT, TerminalPanel::on_stdin_input_enter)
wxEND_EVENT_TABLE()

TerminalPanel::TerminalPanel(wxWindow* parent, KillHandler on_kill,
                              StdinHandler on_stdin)
    : wxPanel(parent, wxID_ANY)
    , flush_timer_(this)
    , kill_handler_(std::move(on_kill))
    , stdin_handler_(std::move(on_stdin))
{
    SetName(ui_names::kTerminalPanel);
    gui::apply_locus_accessible_name(this);

    notebook_ = new wxNotebook(this, wxID_ANY);
    notebook_->SetName(ui_names::kTerminalNotebook);
    gui::apply_locus_accessible_name(notebook_);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(notebook_, 1, wxEXPAND);
    SetSizer(sizer);

    flush_timer_.Start(k_flush_interval_ms);
}

TerminalPanel::~TerminalPanel()
{
    flush_timer_.Stop();
}

void TerminalPanel::set_state(TerminalPanelState* state)
{
    if (state_ == state) return;
    state_ = state;
    rebuild_from_state_();
}

void TerminalPanel::set_first_command_observer(std::function<void()> obs)
{
    first_command_observer_ = std::move(obs);
}

// -- UI thread ---------------------------------------------------------------

void TerminalPanel::on_flush_timer(wxTimerEvent& /*evt*/)
{
    if (!state_) return;

    std::deque<TerminalPanelState::LifeEvent>      lifecycle;
    std::unordered_map<int, std::string>           text;
    if (!state_->drain_pending(lifecycle, text)) return;

    // 1. Lifecycle first -- create / tear down STC pages, update badges. The
    //    state has already applied lifecycle effects to its tab map by the
    //    time drain_pending returns, so this is purely a widget mirror.
    for (const auto& ev : lifecycle) {
        switch (ev.kind) {
            case TerminalPanelState::LifeKind::sync_start: {
                Page* page = ensure_page_(TerminalPanelState::k_sync_id);
                if (!page || !page->stc) break;
                page->stc->SetReadOnly(false);
                page->stc->ClearAll();
                page->stc->SetReadOnly(true);
                page->cmd_header_written = false;
                write_command_header_(*page, ev.command);
                if (auto snap = state_->snapshot(page->id))
                    set_tab_badge_(find_notebook_index_(page->id), *snap);
                if (first_command_observer_) first_command_observer_();
                break;
            }
            case TerminalPanelState::LifeKind::sync_exit: {
                if (auto snap = state_->snapshot(TerminalPanelState::k_sync_id))
                    set_tab_badge_(
                        find_notebook_index_(TerminalPanelState::k_sync_id), *snap);
                break;
            }
            case TerminalPanelState::LifeKind::bg_start: {
                Page* page = ensure_page_(ev.id);
                if (!page) break;
                if (!page->cmd_header_written)
                    write_command_header_(*page, ev.command);
                if (auto snap = state_->snapshot(page->id))
                    set_tab_badge_(find_notebook_index_(page->id), *snap);
                if (first_command_observer_) first_command_observer_();
                break;
            }
            case TerminalPanelState::LifeKind::bg_exit: {
                // Per S5.B: drop the tab when the bg process is gone. The
                // state has already removed the sub-tab from its map; we
                // mirror by deleting the notebook page.
                int idx = find_notebook_index_(ev.id);
                if (idx >= 0) notebook_->DeletePage(static_cast<size_t>(idx));
                pages_.erase(ev.id);
                break;
            }
        }
    }

    // 2. Text chunks. parse_and_append (state side) walks the tab's ANSI
    //    parser and returns the freshly produced events; we write them
    //    straight to the STC for the matching page.
    for (auto& [id, blob] : text) {
        if (blob.empty()) continue;
        Page* page = nullptr;
        if (auto it = pages_.find(id); it != pages_.end()) page = &it->second;
        if (!page || !page->stc) continue;

        auto events = state_->parse_and_append(id, blob);
        if (events.empty()) continue;

        // Track at-bottom-ness BEFORE writing so user scroll position is
        // preserved (matches S5.B behaviour).
        int line_count = page->stc->GetLineCount();
        int visible    = page->stc->LinesOnScreen();
        int first_vis  = page->stc->GetFirstVisibleLine();
        bool at_bottom = (first_vis + visible >= line_count - 1);
        bool stick     = true;
        if (auto snap = state_->snapshot(id)) stick = snap->stick_to_bottom;
        if (at_bottom) {
            stick = true;
            state_->set_stick_to_bottom(id, true);
        } else if (page->stc->GetCurrentPos() != page->stc->GetLastPosition()) {
            stick = false;
            state_->set_stick_to_bottom(id, false);
        }

        page->stc->SetReadOnly(false);
        TerminalTabState dummy;  // unused -- replay uses page only here
        (void)dummy;
        for (const auto& ev : events) apply_ansi_event_(*page, ev, nullptr);
        page->stc->SetReadOnly(true);

        trim_scrollback_(*page);

        if (stick) {
            page->stc->ScrollToLine(page->stc->GetLineCount());
            page->stc->GotoPos(page->stc->GetLastPosition());
        }
    }
}

void TerminalPanel::rebuild_from_state_()
{
    clear_pages_();
    if (!state_) return;

    auto ids = state_->tab_ids_in_order();
    for (int id : ids) {
        auto snap = state_->snapshot(id);
        if (!snap) continue;
        Page* page = ensure_page_(id);
        if (!page) continue;
        // Replay the canonical event log so the user sees exactly what was
        // there when they last visited the tab. We don't write a fresh
        // command header here -- replay covers it if the original sync_start
        // produced one. For bg tabs (which write the header lazily on the
        // first start event), the original header is in the event log too.
        page->cmd_header_written = true;
        auto events = state_->events_clone(id);
        replay_events_(*page, events);
        set_tab_badge_(find_notebook_index_(id), *snap);
    }

    int active = state_->active_sub_tab_id();
    int idx    = find_notebook_index_(active);
    if (idx >= 0) notebook_->SetSelection(static_cast<size_t>(idx));
}

void TerminalPanel::clear_pages_()
{
    if (!notebook_) return;
    while (notebook_->GetPageCount() > 0) {
        notebook_->DeletePage(0);
    }
    pages_.clear();
}

TerminalPanel::Page* TerminalPanel::ensure_page_(int id)
{
    auto it = pages_.find(id);
    if (it != pages_.end()) return &it->second;
    if (!notebook_) return nullptr;

    Page page;
    page.id    = id;
    page.panel = new wxPanel(notebook_, wxID_ANY);
    page.stc   = new wxStyledTextCtrl(page.panel, wxID_ANY);
    ensure_style_table_(page.stc);
    page.stc->SetReadOnly(true);
    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(page.stc, 1, wxEXPAND);

    // S5.Z task 4 -- bg tabs get a single-line stdin input docked below the
    // STC. Sync tab leaves it off: the dispatcher waits synchronously on
    // exit, the user can't realistically feed input mid-run there.
    if (id != TerminalPanelState::k_sync_id) {
        page.stdin_input = new wxTextCtrl(
            page.panel, ID_TERM_STDIN_INPUT, wxEmptyString,
            wxDefaultPosition, wxDefaultSize,
            wxTE_PROCESS_ENTER);
        std::string name = std::string(ui_names::kTerminalStdinInputPrefix) +
                           std::to_string(id);
        page.stdin_input->SetName(name);
        gui::apply_locus_accessible_name(page.stdin_input);
        page.stdin_input->SetHint("type and press Enter to send to stdin");
        sz->Add(page.stdin_input, 0, wxEXPAND | wxTOP, 2);
    }

    page.panel->SetSizer(sz);

    wxString label = (id == TerminalPanelState::k_sync_id)
        ? wxString("Run")
        : wxString::Format("#%d", id);
    notebook_->AddPage(page.panel, label);

    auto [insert_it, _] = pages_.emplace(id, std::move(page));
    return &insert_it->second;
}

int TerminalPanel::find_notebook_index_(int id) const
{
    auto it = pages_.find(id);
    if (it == pages_.end()) return -1;
    wxWindow* w = it->second.panel;
    for (size_t i = 0; i < notebook_->GetPageCount(); ++i)
        if (notebook_->GetPage(i) == w) return static_cast<int>(i);
    return -1;
}

void TerminalPanel::write_command_header_(Page& page, const std::string& command)
{
    if (!page.stc || command.empty()) return;
    page.stc->SetReadOnly(false);
    AnsiStyle style;
    write_styled_(page, "> " + command + "\n", style);
    page.stc->SetReadOnly(true);
    page.cmd_header_written = true;
}

void TerminalPanel::replay_events_(Page& page, const std::vector<AnsiEvent>& events)
{
    if (!page.stc) return;
    page.stc->SetReadOnly(false);
    for (const auto& ev : events) apply_ansi_event_(page, ev, nullptr);
    page.stc->SetReadOnly(true);
    trim_scrollback_(page);
    page.stc->ScrollToLine(page.stc->GetLineCount());
    page.stc->GotoPos(page.stc->GetLastPosition());
}

void TerminalPanel::apply_ansi_event_(Page& page, const AnsiEvent& ev,
                                       TerminalTabState* /*tab_state*/)
{
    switch (ev.kind) {
        case AnsiEventKind::text:
            write_styled_(page, ev.text, ev.style);
            break;
        case AnsiEventKind::erase_to_eol: {
            int pos = page.stc->GetLastPosition();
            int line = page.stc->LineFromPosition(pos);
            int eol  = page.stc->GetLineEndPosition(line);
            if (eol > pos) page.stc->DeleteRange(pos, eol - pos);
            break;
        }
        case AnsiEventKind::erase_to_bol: {
            int pos  = page.stc->GetLastPosition();
            int line = page.stc->LineFromPosition(pos);
            int bol  = page.stc->PositionFromLine(line);
            if (pos > bol) page.stc->DeleteRange(bol, pos - bol);
            break;
        }
        case AnsiEventKind::erase_line: {
            int pos  = page.stc->GetLastPosition();
            int line = page.stc->LineFromPosition(pos);
            int bol  = page.stc->PositionFromLine(line);
            int eol  = page.stc->GetLineEndPosition(line);
            if (eol > bol) page.stc->DeleteRange(bol, eol - bol);
            page.stc->GotoPos(bol);
            break;
        }
        case AnsiEventKind::erase_display:
            page.stc->ClearAll();
            break;
    }
}

void TerminalPanel::write_styled_(Page& page, const std::string& text,
                                   const AnsiStyle& style)
{
    if (text.empty() || !page.stc) return;

    // Robust byte-to-wxString decode (see S5.B for the rationale).
    wxString s = wxString::FromUTF8(text.data(), text.size());
    if (s.IsEmpty() && !text.empty()) {
        s = wxString::From8BitData(text.data(), text.size());
    }

    int start_pos = page.stc->GetLastPosition();
    page.stc->AppendText(s);
    int end_pos = page.stc->GetLastPosition();
    int id      = style_id_for_(style);
    if (end_pos > start_pos) {
        page.stc->StartStyling(start_pos);
        page.stc->SetStyling(end_pos - start_pos, id);
    }
}

void TerminalPanel::trim_scrollback_(Page& page)
{
    if (max_lines_per_tab_ == 0) return;
    int line_count = page.stc->GetLineCount();
    if (line_count <= static_cast<int>(max_lines_per_tab_)) return;
    int excess = line_count - static_cast<int>(max_lines_per_tab_);
    int cut_pos = page.stc->PositionFromLine(excess);
    page.stc->SetReadOnly(false);
    page.stc->DeleteRange(0, cut_pos);
    page.stc->SetReadOnly(true);
}

void TerminalPanel::set_tab_badge_(int idx,
                                    const TerminalPanelState::TabSnapshot& snap)
{
    if (idx < 0 || !notebook_) return;
    std::string label_cmd = snap.command;
    if (label_cmd.size() > 32) label_cmd = label_cmd.substr(0, 29) + "...";
    wxString base = (snap.id == TerminalPanelState::k_sync_id)
        ? wxString("Run")
        : wxString::Format("#%d %s", snap.id, wxString::FromUTF8(label_cmd));
    wxString badge;
    if (snap.active) {
        badge = " *";
    } else if (snap.killed || snap.timed_out) {
        badge = wxString::Format(" [x:%d]", snap.exit_code);
    } else {
        badge = wxString::Format(" [%d]", snap.exit_code);
    }
    notebook_->SetPageText(static_cast<size_t>(idx), base + badge);
}

int TerminalPanel::style_id_for_(const AnsiStyle& s)
{
    if (s.fg_index < 0 && s.bg_index < 0 && !s.bold && !s.dim)
        return k_style_default;
    int fg = s.fg_index < 0 ? 16 : s.fg_index;
    int bg = s.bg_index < 0 ? 16 : s.bg_index;
    int id = 1 + fg + bg * 17 + (s.bold ? 17 * 17 : 0);
    if (id < 1) id = 1;
    if (id >= 32 && id <= 39) id += 8;
    if (id > 254) id = 254;
    return id;
}

void TerminalPanel::ensure_style_table_(wxStyledTextCtrl* stc)
{
    const bool dark = theme::is_dark();
    const wxColour default_bg = dark ? wxColour(30, 30, 30)   : *wxWHITE;
    const wxColour default_fg = dark ? wxColour(220, 220, 220) : *wxBLACK;

    wxFont mono(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Consolas");
    stc->StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
    stc->StyleSetBackground(wxSTC_STYLE_DEFAULT, default_bg);
    stc->StyleSetForeground(wxSTC_STYLE_DEFAULT, default_fg);
    stc->StyleClearAll();
    stc->SetMarginWidth(0, 0);
    stc->SetMarginWidth(1, 0);
    stc->SetWrapMode(wxSTC_WRAP_NONE);
    stc->SetUseHorizontalScrollBar(true);

    std::vector<bool> assigned(256, false);
    for (int bold = 0; bold < 2; ++bold) {
        for (int bg = 0; bg < 17; ++bg) {
            for (int fg = 0; fg < 17; ++fg) {
                AnsiStyle s;
                s.fg_index = fg == 16 ? -1 : static_cast<int8_t>(fg);
                s.bg_index = bg == 16 ? -1 : static_cast<int8_t>(bg);
                s.bold     = (bold != 0);
                int id     = style_id_for_(s);
                if (id == k_style_default) continue;
                if (assigned[static_cast<size_t>(id)]) continue;
                assigned[static_cast<size_t>(id)] = true;
                stc->StyleSetFont(id, mono);
                stc->StyleSetForeground(id,
                    s.fg_index < 0 ? default_fg : palette(s.fg_index));
                stc->StyleSetBackground(id,
                    s.bg_index < 0 ? default_bg : palette(s.bg_index));
                stc->StyleSetBold(id, s.bold);
            }
        }
    }
}

void TerminalPanel::on_stdin_input_enter(wxCommandEvent& evt)
{
    // Find the page whose stdin_input owns this event.
    auto* src = dynamic_cast<wxTextCtrl*>(evt.GetEventObject());
    if (!src) return;
    Page* target = nullptr;
    for (auto& [id, page] : pages_) {
        if (page.stdin_input == src) { target = &page; break; }
    }
    if (!target) return;
    if (target->id == TerminalPanelState::k_sync_id) return;

    wxString line = src->GetValue();
    src->Clear();

    std::string utf8(line.utf8_str().data());
    // Locally echo the user input as a dimmed-grey line so it stands out from
    // program output. Newline appended on send. Note we echo even on failure
    // so the user sees what they tried to submit.
    if (target->stc) {
        AnsiStyle dim_style;
        dim_style.dim     = true;
        dim_style.fg_index = 8;  // bright black / grey
        target->stc->SetReadOnly(false);
        write_styled_(*target, utf8 + "\n", dim_style);
        target->stc->SetReadOnly(true);
        target->stc->ScrollToLine(target->stc->GetLineCount());
        target->stc->GotoPos(target->stc->GetLastPosition());
    }

    if (stdin_handler_) {
        bool ok = stdin_handler_(target->id, utf8 + "\n");
        if (!ok && target->stc) {
            AnsiStyle warn;
            warn.fg_index = 1;  // red
            target->stc->SetReadOnly(false);
            write_styled_(*target,
                          "[locus: failed to write to stdin -- process may have exited]\n",
                          warn);
            target->stc->SetReadOnly(true);
        }
    }
}

void TerminalPanel::on_context_menu_action(wxCommandEvent& evt)
{
    if (!notebook_) return;
    int idx = notebook_->GetSelection();
    if (idx < 0) return;
    int target_id = -1;
    for (auto& [id, page] : pages_) {
        if (find_notebook_index_(id) == idx) { target_id = id; break; }
    }
    if (target_id < 0) return;
    auto pit = pages_.find(target_id);
    if (pit == pages_.end()) return;
    Page& page = pit->second;

    switch (evt.GetId()) {
        case ID_TERM_COPY_ALL: {
            if (page.stc && wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(page.stc->GetText()));
                wxTheClipboard->Close();
            }
            break;
        }
        case ID_TERM_COPY_SEL: {
            if (!page.stc) break;
            wxString sel = page.stc->GetSelectedText();
            if (!sel.IsEmpty() && wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(sel));
                wxTheClipboard->Close();
            }
            break;
        }
        case ID_TERM_CLEAR: {
            if (!page.stc) break;
            page.stc->SetReadOnly(false);
            page.stc->ClearAll();
            page.stc->SetReadOnly(true);
            if (state_) state_->clear_log(target_id);
            break;
        }
        case ID_TERM_KILL: {
            if (target_id != TerminalPanelState::k_sync_id && kill_handler_)
                kill_handler_(target_id);
            break;
        }
    }
}

} // namespace locus
