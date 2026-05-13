#include "terminal_panel.h"
#include "locus_accessible.h"
#include "ui_names.h"

#include "../../tools/process_registry.h"

#include <spdlog/spdlog.h>

#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/clipbrd.h>

#include <algorithm>
#include <cstring>

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

// Style id 0 reserved for STC default; we map AnsiStyle -> id 1..256.
// Encoding: (fg + 1) * 17 + (bg + 1) + bold*2 + dim*1, capped to [1, 256].
// Keeps the style table small while covering the common combinations.
constexpr int k_style_default = 0;

enum {
    ID_TERM_COPY_ALL = wxID_HIGHEST + 1700,
    ID_TERM_COPY_SEL,
    ID_TERM_CLEAR,
    ID_TERM_KILL,
};

} // namespace

wxBEGIN_EVENT_TABLE(TerminalPanel, wxPanel)
    EVT_TIMER(wxID_ANY, TerminalPanel::on_flush_timer)
    EVT_MENU(ID_TERM_COPY_ALL, TerminalPanel::on_context_menu_action)
    EVT_MENU(ID_TERM_COPY_SEL, TerminalPanel::on_context_menu_action)
    EVT_MENU(ID_TERM_CLEAR,    TerminalPanel::on_context_menu_action)
    EVT_MENU(ID_TERM_KILL,     TerminalPanel::on_context_menu_action)
wxEND_EVENT_TABLE()

TerminalPanel::TerminalPanel(wxWindow* parent, ProcessSinkBroker* broker,
                             ProcessRegistry* registry, std::size_t max_lines_per_tab)
    : wxPanel(parent, wxID_ANY)
    , flush_timer_(this)
    , max_lines_per_tab_(max_lines_per_tab)
    , broker_(broker)
    , registry_(registry)
{
    SetName(ui_names::kTerminalPanel);
    gui::apply_locus_accessible_name(this);

    notebook_ = new wxNotebook(this, wxID_ANY);
    notebook_->SetName(ui_names::kTerminalNotebook);
    gui::apply_locus_accessible_name(notebook_);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(notebook_, 1, wxEXPAND);
    SetSizer(sizer);

    // Reserve the sync tab up front -- empty until the first run_command lands.
    auto sync = std::make_unique<Tab>();
    sync->id      = k_sync_id;
    sync->command = "(no command yet)";
    sync->page    = new wxPanel(notebook_, wxID_ANY);
    sync->stc     = new wxStyledTextCtrl(sync->page, wxID_ANY);
    auto* sync_sizer = new wxBoxSizer(wxVERTICAL);
    sync_sizer->Add(sync->stc, 1, wxEXPAND);
    sync->page->SetSizer(sync_sizer);
    ensure_style_table(sync->stc);
    sync->stc->SetReadOnly(true);
    notebook_->AddPage(sync->page, "Run");
    tabs_[k_sync_id] = std::move(sync);

    // Wire ourselves as the sink.
    if (broker_) broker_->set_sink(this);

    flush_timer_.Start(k_flush_interval_ms);
}

TerminalPanel::~TerminalPanel()
{
    detach();
    flush_timer_.Stop();
}

void TerminalPanel::detach()
{
    bool was_attached = attached_.exchange(false);
    if (!was_attached) return;
    if (broker_) broker_->set_sink(nullptr);
    broker_ = nullptr;
}

// -- IProcessSink (worker threads) -------------------------------------------

void TerminalPanel::on_bg_started(int id, const std::string& command)
{
    std::lock_guard l(mu_);
    LifeEvent e;
    e.kind    = LifeKind::bg_start;
    e.id      = id;
    e.command = command;
    pending_lifecycle_.push_back(std::move(e));
}

void TerminalPanel::on_bg_chunk(int id, const char* data, std::size_t n)
{
    std::lock_guard l(mu_);
    pending_text_[id].append(data, n);
}

void TerminalPanel::on_bg_exited(int id, int exit_code, bool killed)
{
    std::lock_guard l(mu_);
    LifeEvent e;
    e.kind      = LifeKind::bg_exit;
    e.id        = id;
    e.exit_code = exit_code;
    e.killed    = killed;
    pending_lifecycle_.push_back(std::move(e));
}

void TerminalPanel::on_sync_started(const std::string& command)
{
    std::lock_guard l(mu_);
    LifeEvent e;
    e.kind    = LifeKind::sync_start;
    e.id      = k_sync_id;
    e.command = command;
    pending_lifecycle_.push_back(std::move(e));
    // Drop any leftover text that hadn't been flushed yet -- this is a brand
    // new invocation and the panel resets the sync tab on the lifecycle event.
    pending_text_.erase(k_sync_id);
}

void TerminalPanel::on_sync_chunk(const char* data, std::size_t n)
{
    std::lock_guard l(mu_);
    pending_text_[k_sync_id].append(data, n);
}

void TerminalPanel::on_sync_exited(int exit_code, bool timed_out)
{
    std::lock_guard l(mu_);
    LifeEvent e;
    e.kind      = LifeKind::sync_exit;
    e.id        = k_sync_id;
    e.exit_code = exit_code;
    e.timed_out = timed_out;
    pending_lifecycle_.push_back(std::move(e));
}

// -- UI thread ---------------------------------------------------------------

void TerminalPanel::on_flush_timer(wxTimerEvent& /*evt*/)
{
    std::deque<LifeEvent>                   lifecycle;
    std::unordered_map<int, std::string>    text;
    {
        std::lock_guard l(mu_);
        lifecycle.swap(pending_lifecycle_);
        text.swap(pending_text_);
    }

    // Lifecycle first -- a start event may need to create a tab before its
    // chunks arrive in the same tick.
    for (const auto& ev : lifecycle) process_lifecycle(ev);

    for (auto& [id, blob] : text) {
        auto it = tabs_.find(id);
        if (it == tabs_.end()) {
            // Process started + chunk + exited in the same window before the
            // start event was processed (unlikely but possible). Drop the
            // unattributable text rather than silently lose tab structure.
            continue;
        }
        append_to_tab(*it->second, blob);
    }
}

TerminalPanel::Tab* TerminalPanel::ensure_tab(int id, const std::string& command)
{
    auto it = tabs_.find(id);
    if (it != tabs_.end()) return it->second.get();

    auto tab = std::make_unique<Tab>();
    tab->id      = id;
    tab->command = command;
    tab->page    = new wxPanel(notebook_, wxID_ANY);
    tab->stc     = new wxStyledTextCtrl(tab->page, wxID_ANY);
    ensure_style_table(tab->stc);
    tab->stc->SetReadOnly(true);
    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(tab->stc, 1, wxEXPAND);
    tab->page->SetSizer(sz);

    // Tab label: short id + truncated command.
    std::string label_cmd = command;
    if (label_cmd.size() > 32) label_cmd = label_cmd.substr(0, 29) + "...";
    wxString label = wxString::Format("#%d %s", id, wxString::FromUTF8(label_cmd));
    notebook_->AddPage(tab->page, label);

    Tab* raw = tab.get();
    tabs_[id] = std::move(tab);
    return raw;
}

int TerminalPanel::find_tab_index(int id) const
{
    auto it = tabs_.find(id);
    if (it == tabs_.end()) return -1;
    wxWindow* page = it->second->page;
    for (size_t i = 0; i < notebook_->GetPageCount(); ++i)
        if (notebook_->GetPage(i) == page) return static_cast<int>(i);
    return -1;
}

void TerminalPanel::set_tab_badge(int idx, const Tab& tab)
{
    if (idx < 0) return;
    std::string label_cmd = tab.command;
    if (label_cmd.size() > 32) label_cmd = label_cmd.substr(0, 29) + "...";
    wxString base = tab.id == k_sync_id
        ? wxString("Run")
        : wxString::Format("#%d %s", tab.id, wxString::FromUTF8(label_cmd));
    wxString badge;
    if (tab.active) {
        badge = " *";   // running
    } else if (tab.killed || tab.timed_out) {
        badge = wxString::Format(" [x:%d]", tab.exit_code);
    } else {
        badge = wxString::Format(" [%d]", tab.exit_code);
    }
    notebook_->SetPageText(idx, base + badge);
}

void TerminalPanel::process_lifecycle(const LifeEvent& ev)
{
    switch (ev.kind) {
        case LifeKind::sync_start: {
            auto it = tabs_.find(k_sync_id);
            if (it == tabs_.end()) return;
            Tab& tab = *it->second;
            tab.command   = ev.command;
            tab.active    = true;
            tab.killed    = false;
            tab.timed_out = false;
            tab.exit_code = 0;
            tab.parser.reset();
            tab.stc->SetReadOnly(false);
            tab.stc->ClearAll();
            tab.stc->SetReadOnly(true);
            tab.stick_to_bottom = true;
            set_tab_badge(find_tab_index(k_sync_id), tab);
            break;
        }
        case LifeKind::sync_exit: {
            auto it = tabs_.find(k_sync_id);
            if (it == tabs_.end()) return;
            Tab& tab = *it->second;
            tab.active    = false;
            tab.exit_code = ev.exit_code;
            tab.timed_out = ev.timed_out;
            set_tab_badge(find_tab_index(k_sync_id), tab);
            break;
        }
        case LifeKind::bg_start: {
            Tab* tab = ensure_tab(ev.id, ev.command);
            tab->active = true;
            set_tab_badge(find_tab_index(ev.id), *tab);
            break;
        }
        case LifeKind::bg_exit: {
            auto it = tabs_.find(ev.id);
            if (it == tabs_.end()) return;
            Tab& tab = *it->second;
            tab.active    = false;
            tab.exit_code = ev.exit_code;
            tab.killed    = ev.killed;
            set_tab_badge(find_tab_index(ev.id), tab);
            break;
        }
    }
}

void TerminalPanel::append_to_tab(Tab& tab, const std::string& text)
{
    if (text.empty() || !tab.stc) return;

    // Detect whether the user has scrolled away from the bottom. If they
    // have, keep stick_to_bottom off; if they're at the bottom (within one
    // line), turn it back on.
    int line_count = tab.stc->GetLineCount();
    int visible    = tab.stc->LinesOnScreen();
    int first_vis  = tab.stc->GetFirstVisibleLine();
    bool at_bottom = (first_vis + visible >= line_count - 1);
    if (at_bottom) tab.stick_to_bottom = true;
    else if (!at_bottom && tab.stc->GetCurrentPos() != tab.stc->GetLastPosition()) {
        // user scrolled away -- keep current position
        tab.stick_to_bottom = false;
    }

    std::vector<AnsiEvent> events;
    tab.parser.consume(text.data(), text.size(), events);
    tab.stc->SetReadOnly(false);
    for (const auto& ev : events) apply_ansi_event(tab, ev);
    tab.stc->SetReadOnly(true);

    trim_scrollback(tab);

    if (tab.stick_to_bottom) {
        tab.stc->ScrollToLine(tab.stc->GetLineCount());
        tab.stc->GotoPos(tab.stc->GetLastPosition());
    }
}

void TerminalPanel::trim_scrollback(Tab& tab)
{
    if (max_lines_per_tab_ == 0) return;
    int line_count = tab.stc->GetLineCount();
    if (line_count <= static_cast<int>(max_lines_per_tab_)) return;
    int excess = line_count - static_cast<int>(max_lines_per_tab_);
    int cut_pos = tab.stc->PositionFromLine(excess);
    tab.stc->SetReadOnly(false);
    tab.stc->DeleteRange(0, cut_pos);
    tab.stc->SetReadOnly(true);
}

void TerminalPanel::apply_ansi_event(Tab& tab, const AnsiEvent& ev)
{
    switch (ev.kind) {
        case AnsiEventKind::text:
            write_styled(tab, ev.text, ev.style);
            break;
        case AnsiEventKind::erase_to_eol: {
            int pos = tab.stc->GetLastPosition();
            int line = tab.stc->LineFromPosition(pos);
            int eol  = tab.stc->GetLineEndPosition(line);
            if (eol > pos) tab.stc->DeleteRange(pos, eol - pos);
            break;
        }
        case AnsiEventKind::erase_to_bol: {
            int pos  = tab.stc->GetLastPosition();
            int line = tab.stc->LineFromPosition(pos);
            int bol  = tab.stc->PositionFromLine(line);
            if (pos > bol) tab.stc->DeleteRange(bol, pos - bol);
            break;
        }
        case AnsiEventKind::erase_line: {
            int pos  = tab.stc->GetLastPosition();
            int line = tab.stc->LineFromPosition(pos);
            int bol  = tab.stc->PositionFromLine(line);
            int eol  = tab.stc->GetLineEndPosition(line);
            if (eol > bol) tab.stc->DeleteRange(bol, eol - bol);
            tab.stc->GotoPos(bol);
            break;
        }
        case AnsiEventKind::erase_display:
            tab.stc->ClearAll();
            break;
    }
}

// Encode an AnsiStyle as a style table index in [1, 255].
int TerminalPanel::style_id_for(const AnsiStyle& s)
{
    if (s.fg_index < 0 && s.bg_index < 0 && !s.bold && !s.dim)
        return k_style_default;
    // 4 bits bold|dim|fg_default|bg_default (top), 4 bits fg_index, 4 bits bg_index
    int fg = s.fg_index < 0 ? 16 : s.fg_index;  // 0..16 -> 5 bits
    int bg = s.bg_index < 0 ? 16 : s.bg_index;
    int id = 1 + fg + bg * 17 + (s.bold ? 17 * 17 : 0);
    if (id < 1) id = 1;
    if (id > 254) id = 254;  // STC reserves a few high ids; stay clear
    return id;
}

void TerminalPanel::ensure_style_table(wxStyledTextCtrl* stc)
{
    // Monospace font + base colours.
    wxFont mono(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Consolas");
    stc->StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
    stc->StyleSetBackground(wxSTC_STYLE_DEFAULT, *wxWHITE);
    stc->StyleSetForeground(wxSTC_STYLE_DEFAULT, *wxBLACK);
    stc->StyleClearAll();
    stc->SetMarginWidth(0, 0);
    stc->SetMarginWidth(1, 0);
    stc->SetWrapMode(wxSTC_WRAP_NONE);
    stc->SetUseHorizontalScrollBar(true);

    // Pre-populate the cross product of (fg, bg, bold). dim is collapsed onto
    // bold for now -- few real terminals distinguish them and the visual
    // difference on a white background is tiny.
    for (int bold = 0; bold < 2; ++bold) {
        for (int bg = 0; bg < 17; ++bg) {
            for (int fg = 0; fg < 17; ++fg) {
                AnsiStyle s;
                s.fg_index = fg == 16 ? -1 : static_cast<int8_t>(fg);
                s.bg_index = bg == 16 ? -1 : static_cast<int8_t>(bg);
                s.bold     = (bold != 0);
                int id = style_id_for(s);
                if (id == k_style_default) continue;
                stc->StyleSetFont(id, mono);
                stc->StyleSetForeground(id,
                    s.fg_index < 0 ? *wxBLACK : palette(s.fg_index));
                stc->StyleSetBackground(id,
                    s.bg_index < 0 ? *wxWHITE : palette(s.bg_index));
                stc->StyleSetBold(id, s.bold);
            }
        }
    }
}

void TerminalPanel::write_styled(Tab& tab, const std::string& text,
                                  const AnsiStyle& style)
{
    if (text.empty()) return;
    int start_pos = tab.stc->GetLastPosition();
    tab.stc->AppendText(wxString::FromUTF8(text));
    int end_pos = tab.stc->GetLastPosition();
    int id = style_id_for(style);
    if (end_pos > start_pos) {
        tab.stc->StartStyling(start_pos);
        tab.stc->SetStyling(end_pos - start_pos, id);
    }
}

void TerminalPanel::on_tab_right_click(wxContextMenuEvent& /*evt*/)
{
    // Context menu wiring is deferred until the inner STCs each route their
    // wxEVT_CONTEXT_MENU here. For v1 the right-click menu lives on the
    // notebook tabs via wxNotebook's native handling.
}

void TerminalPanel::on_context_menu_action(wxCommandEvent& evt)
{
    int idx = notebook_->GetSelection();
    if (idx < 0) return;
    int target_id = -1;
    for (auto& [id, tab] : tabs_) {
        if (find_tab_index(id) == idx) { target_id = id; break; }
    }
    if (target_id < 0) return;
    auto it = tabs_.find(target_id);
    if (it == tabs_.end()) return;
    Tab& tab = *it->second;

    switch (evt.GetId()) {
        case ID_TERM_COPY_ALL: {
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(tab.stc->GetText()));
                wxTheClipboard->Close();
            }
            break;
        }
        case ID_TERM_COPY_SEL: {
            wxString sel = tab.stc->GetSelectedText();
            if (!sel.IsEmpty() && wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(sel));
                wxTheClipboard->Close();
            }
            break;
        }
        case ID_TERM_CLEAR: {
            tab.stc->SetReadOnly(false);
            tab.stc->ClearAll();
            tab.stc->SetReadOnly(true);
            tab.parser.reset();
            break;
        }
        case ID_TERM_KILL: {
            if (registry_ && target_id != k_sync_id && tab.active) {
                registry_->stop(target_id);
            }
            break;
        }
    }
}

} // namespace locus
