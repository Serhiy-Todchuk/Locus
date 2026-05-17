#include "memory_bank_panel.h"

#include "locus_accessible.h"
#include "theme.h"
#include "ui_names.h"

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <set>
#include <sstream>

namespace locus {

namespace {

constexpr int k_debounce_ms = 200;

wxString format_short_time(std::int64_t secs)
{
    if (secs <= 0) return wxString();
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min);
    return wxString::FromAscii(buf);
}

wxString single_line_preview(const std::string& s, std::size_t cap)
{
    std::string norm;
    norm.reserve(s.size());
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!norm.empty() && norm.back() != ' ') norm.push_back(' ');
        } else norm.push_back(c);
    }
    while (!norm.empty() && norm.front() == ' ') norm.erase(norm.begin());
    if (norm.size() > cap) {
        norm.resize(cap > 1 ? cap - 1 : 0);
        norm.push_back(0xE2);  // utf-8 horizontal-ellipsis bytes
        norm.push_back(0x80);
        norm.push_back(0xA6);
    }
    return wxString::FromUTF8(norm);
}

wxString join_tags(const std::vector<std::string>& tags)
{
    std::string s;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) s += ", ";
        s += tags[i];
    }
    return wxString::FromUTF8(s);
}

std::vector<std::string> split_tag_input(const wxString& text)
{
    std::vector<std::string> out;
    std::string buf = text.ToStdString();
    std::string tok;
    auto push = [&] {
        auto a = tok.find_first_not_of(" \t");
        if (a == std::string::npos) { tok.clear(); return; }
        auto b = tok.find_last_not_of(" \t");
        out.push_back(tok.substr(a, b - a + 1));
        tok.clear();
    };
    for (char c : buf) {
        if (c == ',' || c == '\n') push();
        else tok.push_back(c);
    }
    push();
    return out;
}

// Sort the live (non-deleted) snapshot: pinned first, then last_used desc.
// Centralised so live + filtered views agree.
void sort_live(std::vector<MemoryStore::Entry>& v)
{
    sort_entries(v, MemorySort::pinned_first_last_used);
}

} // namespace

// ----------------------------------------------------------------------------
// Construction
// ----------------------------------------------------------------------------

MemoryBankPanel::MemoryBankPanel(wxWindow* parent, MemoryStore* store)
    : wxPanel(parent, wxID_ANY)
    , store_(store)
    , debounce_timer_(this)
{
    SetName(ui_names::kMemoryBankPanel);
    gui::apply_locus_accessible_name(this);

    auto* top = new wxBoxSizer(wxVERTICAL);
    build_toolbar(this, top);

    splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
        wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3DSASH);
    auto* list_pane   = new wxPanel(splitter_, wxID_ANY);
    auto* detail_root = new wxPanel(splitter_, wxID_ANY);

    auto* lsz = new wxBoxSizer(wxVERTICAL);
    build_list(list_pane, lsz);
    list_pane->SetSizer(lsz);

    auto* dsz = new wxBoxSizer(wxVERTICAL);
    build_detail(detail_root, dsz);
    detail_root->SetSizer(dsz);

    splitter_->SplitHorizontally(list_pane, detail_root, -200);
    splitter_->SetMinimumPaneSize(80);
    top->Add(splitter_, 1, wxEXPAND);
    SetSizer(top);

    Bind(wxEVT_TIMER, &MemoryBankPanel::on_debounce_timer, this);

    refresh();
    apply_capability_state();
}

// ----------------------------------------------------------------------------
// Layout
// ----------------------------------------------------------------------------

void MemoryBankPanel::build_toolbar(wxWindow* parent, wxSizer* outer)
{
    auto* row1 = new wxBoxSizer(wxHORIZONTAL);

    auto* search_label = new wxStaticText(parent, wxID_ANY, "Search:");
    search_box_ = new wxTextCtrl(parent, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    search_box_->SetName(ui_names::kMemoryBankSearch);
    gui::apply_locus_accessible_name(search_box_);
    search_box_->SetHint("Hybrid search (FTS + semantic + recency)");
    search_box_->Bind(wxEVT_TEXT,
        [this](wxCommandEvent& e) { (void)e; schedule_refresh(); });

    row1->Add(search_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    row1->Add(search_box_,  1, wxEXPAND | wxRIGHT, 8);

    auto* source_label = new wxStaticText(parent, wxID_ANY, "Source:");
    source_choice_ = new wxChoice(parent, wxID_ANY);
    source_choice_->SetName(ui_names::kMemoryBankSourceChoice);
    gui::apply_locus_accessible_name(source_choice_);
    source_choice_->Append("all");
    source_choice_->Append("user");
    source_choice_->Append("agent");
    source_choice_->Append("mined");
    source_choice_->SetSelection(0);
    source_choice_->Bind(wxEVT_CHOICE, &MemoryBankPanel::on_filter_changed, this);

    row1->Add(source_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    row1->Add(source_choice_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    auto* tag_label = new wxStaticText(parent, wxID_ANY, "Tag:");
    tag_choice_ = new wxChoice(parent, wxID_ANY);
    tag_choice_->SetName(ui_names::kMemoryBankTagChoice);
    gui::apply_locus_accessible_name(tag_choice_);
    tag_choice_->Append("(any)");
    tag_choice_->SetSelection(0);
    tag_choice_->Bind(wxEVT_CHOICE, &MemoryBankPanel::on_filter_changed, this);

    row1->Add(tag_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    row1->Add(tag_choice_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    pinned_only_cb_ = new wxCheckBox(parent, wxID_ANY, "Pinned only");
    pinned_only_cb_->SetName(ui_names::kMemoryBankPinnedOnly);
    gui::apply_locus_accessible_name(pinned_only_cb_);
    pinned_only_cb_->Bind(wxEVT_CHECKBOX, &MemoryBankPanel::on_filter_changed, this);
    row1->Add(pinned_only_cb_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    show_deleted_cb_ = new wxCheckBox(parent, wxID_ANY, "Show deleted");
    show_deleted_cb_->SetName(ui_names::kMemoryBankShowDeleted);
    gui::apply_locus_accessible_name(show_deleted_cb_);
    show_deleted_cb_->Bind(wxEVT_CHECKBOX,
        &MemoryBankPanel::on_show_deleted_toggled, this);
    row1->Add(show_deleted_cb_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    clear_btn_ = new wxButton(parent, wxID_ANY, "Clear");
    clear_btn_->SetName(ui_names::kMemoryBankClearBtn);
    gui::apply_locus_accessible_name(clear_btn_);
    clear_btn_->Bind(wxEVT_BUTTON, &MemoryBankPanel::on_clear_filters, this);
    row1->Add(clear_btn_, 0, wxALIGN_CENTER_VERTICAL);

    outer->Add(row1, 0, wxEXPAND | wxALL, 4);

    auto* row2 = new wxBoxSizer(wxHORIZONTAL);

    delete_btn_ = new wxButton(parent, wxID_ANY, "Delete");
    delete_btn_->SetName(ui_names::kMemoryBankDeleteBtn);
    gui::apply_locus_accessible_name(delete_btn_);
    delete_btn_->Bind(wxEVT_BUTTON, &MemoryBankPanel::on_delete_selected, this);
    row2->Add(delete_btn_, 0, wxRIGHT, 4);

    pin_btn_ = new wxButton(parent, wxID_ANY, "Pin/Unpin");
    pin_btn_->SetName(ui_names::kMemoryBankPinBtn);
    gui::apply_locus_accessible_name(pin_btn_);
    pin_btn_->Bind(wxEVT_BUTTON, &MemoryBankPanel::on_pin_toggle_selected, this);
    row2->Add(pin_btn_, 0, wxRIGHT, 4);

    tag_btn_ = new wxButton(parent, wxID_ANY, "Add tags...");
    tag_btn_->SetName(ui_names::kMemoryBankTagBtn);
    gui::apply_locus_accessible_name(tag_btn_);
    tag_btn_->Bind(wxEVT_BUTTON, &MemoryBankPanel::on_tag_selected, this);
    row2->Add(tag_btn_, 0, wxRIGHT, 4);

    restore_btn_ = new wxButton(parent, wxID_ANY, "Restore");
    restore_btn_->SetName(ui_names::kMemoryBankRestoreBtn);
    gui::apply_locus_accessible_name(restore_btn_);
    restore_btn_->Bind(wxEVT_BUTTON, &MemoryBankPanel::on_restore_selected, this);
    restore_btn_->Hide();    // only visible in show_deleted_ mode
    row2->Add(restore_btn_, 0, wxRIGHT, 4);

    outer->Add(row2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
}

void MemoryBankPanel::build_list(wxWindow* parent, wxSizer* sizer)
{
    list_ = new wxDataViewListCtrl(parent, wxID_ANY, wxDefaultPosition,
        wxDefaultSize,
        wxDV_MULTIPLE | wxDV_ROW_LINES);
    list_->SetName(ui_names::kMemoryBankList);
    gui::apply_locus_accessible_name(list_);
    list_->AppendTextColumn("Pin",      wxDATAVIEW_CELL_INERT,  40,
                            wxALIGN_CENTER);
    list_->AppendTextColumn("Content",  wxDATAVIEW_CELL_INERT, 320);
    list_->AppendTextColumn("Tags",     wxDATAVIEW_CELL_INERT, 130);
    list_->AppendTextColumn("Source",   wxDATAVIEW_CELL_INERT,  60);
    list_->AppendTextColumn("Created",  wxDATAVIEW_CELL_INERT, 120);
    list_->AppendTextColumn("Last used",wxDATAVIEW_CELL_INERT, 120);

    list_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
                &MemoryBankPanel::on_row_selected, this);
    list_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
                &MemoryBankPanel::on_row_activated, this);
    list_->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU,
                &MemoryBankPanel::on_row_right_click, this);

    sizer->Add(list_, 1, wxEXPAND);
}

void MemoryBankPanel::build_detail(wxWindow* parent, wxSizer* sizer)
{
    detail_panel_ = static_cast<wxPanel*>(parent);

    detail_meta_ = new wxStaticText(parent, wxID_ANY, "(no selection)");
    sizer->Add(detail_meta_, 0, wxEXPAND | wxALL, 4);

    detail_content_ = new wxTextCtrl(parent, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
    detail_content_->SetName(ui_names::kMemoryBankDetail);
    gui::apply_locus_accessible_name(detail_content_);
    sizer->Add(detail_content_, 1, wxEXPAND | wxLEFT | wxRIGHT, 4);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* tag_label = new wxStaticText(parent, wxID_ANY, "Tags:");
    detail_tags_ = new wxTextCtrl(parent, wxID_ANY, wxEmptyString);
    detail_pinned_ = new wxCheckBox(parent, wxID_ANY, "Pinned");
    save_btn_ = new wxButton(parent, wxID_ANY, "Save");
    save_btn_->SetName(ui_names::kMemoryBankSaveBtn);
    gui::apply_locus_accessible_name(save_btn_);
    save_btn_->Bind(wxEVT_BUTTON, &MemoryBankPanel::on_save_detail, this);

    row->Add(tag_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    row->Add(detail_tags_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    row->Add(detail_pinned_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    row->Add(save_btn_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(row, 0, wxEXPAND | wxALL, 4);

    clear_detail();
}

// ----------------------------------------------------------------------------
// Capability state
// ----------------------------------------------------------------------------

void MemoryBankPanel::apply_capability_state()
{
    bool enabled = (store_ != nullptr);
    auto disable_kids = [enabled](wxWindow* w) {
        if (w) w->Enable(enabled);
    };
    disable_kids(search_box_);
    disable_kids(source_choice_);
    disable_kids(tag_choice_);
    disable_kids(pinned_only_cb_);
    disable_kids(show_deleted_cb_);
    disable_kids(clear_btn_);
    disable_kids(delete_btn_);
    disable_kids(pin_btn_);
    disable_kids(tag_btn_);
    disable_kids(restore_btn_);
    disable_kids(detail_content_);
    disable_kids(detail_tags_);
    disable_kids(detail_pinned_);
    disable_kids(save_btn_);
    if (!enabled && detail_meta_) {
        detail_meta_->SetLabel(
            "Memory bank is disabled. Enable it in Settings -> "
            "Capabilities to use this panel.");
    }
}

void MemoryBankPanel::set_store(MemoryStore* store)
{
    store_ = store;
    apply_capability_state();
    refresh();
}

// ----------------------------------------------------------------------------
// Refresh + render
// ----------------------------------------------------------------------------

void MemoryBankPanel::schedule_refresh()
{
    if (!debounce_timer_.IsRunning())
        debounce_timer_.StartOnce(k_debounce_ms);
}

void MemoryBankPanel::on_debounce_timer(wxTimerEvent&) { refresh(); }

void MemoryBankPanel::refresh()
{
    if (!store_) {
        rows_.clear();
        deleted_rows_.clear();
        render_list();
        return;
    }

    if (show_deleted_) {
        deleted_rows_ = list_deleted_stubs(*store_);
        rows_.clear();
    } else {
        deleted_rows_.clear();
        rows_ = store_->list_all();
        populate_tag_filter();   // refresh the dropdown contents

        MemoryFilter filter;
        filter.query       = search_box_->GetValue().ToStdString();
        filter.pinned_only = pinned_only_cb_->IsChecked();
        int src_sel = source_choice_->GetSelection();
        if (src_sel > 0) {
            filter.source = source_choice_->GetString(src_sel).ToStdString();
        }
        int tag_sel = tag_choice_->GetSelection();
        if (tag_sel > 0) {
            filter.tags.push_back(tag_choice_->GetString(tag_sel).ToStdString());
        }
        rows_ = apply_filter(rows_, filter);
        sort_live(rows_);
    }
    render_list();

    // Re-render detail pane for current selection if still present.
    if (!current_detail_id_.empty()) {
        bool still_visible = false;
        for (auto& e : rows_)
            if (e.id == current_detail_id_) { still_visible = true; break; }
        if (still_visible) render_detail_for(current_detail_id_);
        else                clear_detail();
    }
}

void MemoryBankPanel::populate_tag_filter()
{
    // Preserve current selection if the tag still exists.
    wxString current = tag_choice_->GetStringSelection();
    tag_choice_->Clear();
    tag_choice_->Append("(any)");
    auto tags = collect_unique_tags(rows_);
    for (auto& t : tags) tag_choice_->Append(wxString::FromUTF8(t));
    int idx = (current.empty() || current == "(any)")
                ? 0
                : tag_choice_->FindString(current);
    tag_choice_->SetSelection(idx < 0 ? 0 : idx);
}

void MemoryBankPanel::render_list()
{
    list_->DeleteAllItems();
    if (show_deleted_) {
        for (auto& d : deleted_rows_) {
            wxVector<wxVariant> v;
            v.push_back(wxVariant(wxString()));
            v.push_back(wxVariant(wxString::FromUTF8(d.content_preview)));
            v.push_back(wxVariant(join_tags(d.tags)));
            v.push_back(wxVariant(wxString()));        // source unknown from stub
            v.push_back(wxVariant(wxString()));
            v.push_back(wxVariant(format_short_time(d.deleted_at_proxy)));
            list_->AppendItem(v, wxUIntPtr(0));
        }
    } else {
        for (auto& e : rows_) {
            wxVector<wxVariant> v;
            v.push_back(wxVariant(wxString(e.pinned ? wxString::FromUTF8("★") : wxString())));
            v.push_back(wxVariant(single_line_preview(e.content, 80)));
            v.push_back(wxVariant(join_tags(e.tags)));
            v.push_back(wxVariant(wxString::FromUTF8(e.source)));
            v.push_back(wxVariant(format_short_time(e.created_at)));
            v.push_back(wxVariant(format_short_time(e.last_used_at)));
            list_->AppendItem(v, wxUIntPtr(0));
        }
    }
}

void MemoryBankPanel::render_detail_for(const std::string& id)
{
    if (!store_) return;
    auto e = store_->get(id);
    if (!e) { clear_detail(); return; }
    current_detail_id_ = id;
    std::ostringstream meta;
    meta << "memory:" << e->id
         << "  source=" << e->source
         << "  created=" << format_short_time(e->created_at).ToStdString()
         << "  last_used=" << format_short_time(e->last_used_at).ToStdString();
    detail_meta_->SetLabel(wxString::FromUTF8(meta.str()));
    detail_content_->ChangeValue(wxString::FromUTF8(e->content));
    detail_tags_->ChangeValue(join_tags(e->tags));
    detail_pinned_->SetValue(e->pinned);
}

void MemoryBankPanel::clear_detail()
{
    current_detail_id_.clear();
    if (detail_meta_)    detail_meta_->SetLabel("(no selection)");
    if (detail_content_) detail_content_->ChangeValue(wxEmptyString);
    if (detail_tags_)    detail_tags_->ChangeValue(wxEmptyString);
    if (detail_pinned_)  detail_pinned_->SetValue(false);
}

// ----------------------------------------------------------------------------
// Selection helpers
// ----------------------------------------------------------------------------

std::vector<std::string> MemoryBankPanel::selected_ids() const
{
    std::vector<std::string> out;
    wxDataViewItemArray sel;
    list_->GetSelections(sel);
    for (auto& it : sel) {
        int row = list_->ItemToRow(it);
        if (row < 0) continue;
        if (show_deleted_) {
            if (row < static_cast<int>(deleted_rows_.size()))
                out.push_back(deleted_rows_[row].id);
        } else {
            if (row < static_cast<int>(rows_.size()))
                out.push_back(rows_[row].id);
        }
    }
    return out;
}

std::optional<std::string> MemoryBankPanel::single_selected_id() const
{
    auto ids = selected_ids();
    if (ids.size() != 1) return std::nullopt;
    return ids.front();
}

// ----------------------------------------------------------------------------
// Event handlers
// ----------------------------------------------------------------------------

void MemoryBankPanel::on_search_changed(wxCommandEvent&) { schedule_refresh(); }

void MemoryBankPanel::on_filter_changed(wxCommandEvent&) { refresh(); }

void MemoryBankPanel::on_clear_filters(wxCommandEvent&)
{
    search_box_->ChangeValue(wxEmptyString);
    source_choice_->SetSelection(0);
    tag_choice_->SetSelection(0);
    pinned_only_cb_->SetValue(false);
    refresh();
}

void MemoryBankPanel::on_show_deleted_toggled(wxCommandEvent&)
{
    show_deleted_ = show_deleted_cb_->IsChecked();
    // Filter widgets are not meaningful for the deleted view; disable them
    // visually but don't reset state -- toggling back restores prior filters.
    bool live = !show_deleted_;
    if (search_box_)     search_box_->Enable(live);
    if (source_choice_)  source_choice_->Enable(live);
    if (tag_choice_)     tag_choice_->Enable(live);
    if (pinned_only_cb_) pinned_only_cb_->Enable(live);
    if (clear_btn_)      clear_btn_->Enable(live);
    if (pin_btn_)        pin_btn_->Enable(live);
    if (tag_btn_)        tag_btn_->Enable(live);
    if (save_btn_)       save_btn_->Enable(live);
    if (detail_content_) detail_content_->Enable(live);
    if (detail_tags_)    detail_tags_->Enable(live);
    if (detail_pinned_)  detail_pinned_->Enable(live);
    if (restore_btn_) {
        restore_btn_->Show(show_deleted_);
        restore_btn_->GetParent()->Layout();
    }
    clear_detail();
    refresh();
}

void MemoryBankPanel::on_row_selected(wxDataViewEvent&)
{
    if (show_deleted_) { clear_detail(); return; }
    auto id = single_selected_id();
    if (id) render_detail_for(*id);
    else    clear_detail();
}

void MemoryBankPanel::on_row_activated(wxDataViewEvent&)
{
    if (show_deleted_) return;
    if (detail_content_) detail_content_->SetFocus();
}

void MemoryBankPanel::on_row_right_click(wxDataViewEvent&)
{
    if (!store_) return;

    enum {
        MID_DELETE = 8500, MID_PIN, MID_TAG, MID_VIEW,
        MID_COPY, MID_RESTORE, MID_HARD_DELETE
    };
    wxMenu m;
    if (show_deleted_) {
        m.Append(MID_RESTORE,      "Restore");
        m.Append(MID_HARD_DELETE,  "Delete permanently");
    } else {
        m.Append(MID_VIEW,    "View / Edit");
        m.Append(MID_PIN,     "Pin / Unpin");
        m.Append(MID_TAG,     "Add tags...");
        m.AppendSeparator();
        m.Append(MID_COPY,    "Copy content");
        m.Append(MID_DELETE,  "Delete");
    }
    int picked = GetPopupMenuSelectionFromUser(m);

    wxCommandEvent dummy;
    switch (picked) {
    case MID_DELETE:       on_delete_selected(dummy); break;
    case MID_PIN:          on_pin_toggle_selected(dummy); break;
    case MID_TAG:          on_tag_selected(dummy); break;
    case MID_VIEW: {
        if (auto id = single_selected_id()) render_detail_for(*id);
        if (detail_content_) detail_content_->SetFocus();
        break;
    }
    case MID_COPY: {
        if (auto id = single_selected_id()) {
            if (auto e = store_->get(*id); e && wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(
                    wxString::FromUTF8(e->content)));
                wxTheClipboard->Close();
            }
        }
        break;
    }
    case MID_RESTORE:      on_restore_selected(dummy); break;
    case MID_HARD_DELETE: {
        auto ids = selected_ids();
        if (ids.empty()) break;
        int ans = wxMessageBox(
            wxString::Format("Permanently delete %zu entry(s)? "
                             "This cannot be undone.", ids.size()),
            "Delete permanently", wxYES_NO | wxICON_WARNING, this);
        if (ans != wxYES) break;
        for (auto& id : ids) (void)store_->hard_delete(id);
        refresh();
        break;
    }
    default: break;
    }
}

bool MemoryBankPanel::confirm_bulk_delete(const std::vector<std::string>& ids)
{
    if (ids.empty()) return false;
    wxString first_preview;
    if (!ids.empty()) {
        if (auto e = store_->get(ids.front())) {
            first_preview = single_line_preview(e->content, 60);
        }
    }
    wxString msg = wxString::Format(
        "Delete %zu memory entry(s)?\n\nFirst: \"%s\"\n\n"
        "Entries will be soft-deleted (30-day retention).",
        ids.size(), first_preview);
    return wxMessageBox(msg, "Delete memory",
                        wxYES_NO | wxICON_QUESTION, this) == wxYES;
}

void MemoryBankPanel::on_delete_selected(wxCommandEvent&)
{
    if (!store_) return;
    auto ids = selected_ids();
    if (ids.empty()) {
        wxMessageBox("Select at least one memory entry to delete.",
                     "Delete memory", wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (!confirm_bulk_delete(ids)) return;
    int deleted = 0;
    for (auto& id : ids) {
        if (store_->soft_delete(id)) ++deleted;
    }
    spdlog::info("MemoryBankPanel: soft-deleted {} entry(s)", deleted);
    clear_detail();
    refresh();
}

void MemoryBankPanel::on_pin_toggle_selected(wxCommandEvent&)
{
    if (!store_) return;
    auto ids = selected_ids();
    if (ids.empty()) return;
    // Determine the new state: if ANY selected entry is unpinned, pin all;
    // otherwise unpin all. Matches the user's mental model of "make these
    // pinned".
    bool any_unpinned = false;
    for (auto& id : ids) {
        if (auto e = store_->get(id); e && !e->pinned) {
            any_unpinned = true; break;
        }
    }
    bool target = any_unpinned;
    for (auto& id : ids) {
        store_->update(id, std::nullopt, std::nullopt, target);
    }
    refresh();
}

void MemoryBankPanel::on_tag_selected(wxCommandEvent&)
{
    if (!store_) return;
    auto ids = selected_ids();
    if (ids.empty()) return;
    wxTextEntryDialog dlg(this,
        "Comma-separated tags to add to the selected entries:",
        "Add tags", wxEmptyString);
    if (dlg.ShowModal() != wxID_OK) return;
    auto added = split_tag_input(dlg.GetValue());
    if (added.empty()) return;
    for (auto& id : ids) {
        auto e = store_->get(id);
        if (!e) continue;
        std::set<std::string> merged(e->tags.begin(), e->tags.end());
        for (auto& t : added) merged.insert(t);
        std::vector<std::string> next(merged.begin(), merged.end());
        store_->update(id, std::nullopt, std::move(next), std::nullopt);
    }
    refresh();
}

void MemoryBankPanel::on_save_detail(wxCommandEvent&)
{
    if (!store_ || current_detail_id_.empty()) return;
    auto content = detail_content_->GetValue().ToStdString();
    auto tags    = split_tag_input(detail_tags_->GetValue());
    bool pinned  = detail_pinned_->IsChecked();
    if (!store_->update(current_detail_id_, content, tags, pinned)) {
        wxMessageBox("Failed to save (entry not found or content empty).",
                     "Memory bank", wxOK | wxICON_WARNING, this);
        return;
    }
    refresh();
}

void MemoryBankPanel::on_restore_selected(wxCommandEvent&)
{
    if (!store_) return;
    auto ids = selected_ids();
    if (ids.empty()) return;
    int restored = 0;
    for (auto& id : ids) if (store_->restore_deleted(id)) ++restored;
    spdlog::info("MemoryBankPanel: restored {} entry(s)", restored);
    refresh();
}

// ----------------------------------------------------------------------------
// Activity-event hook
// ----------------------------------------------------------------------------

void MemoryBankPanel::on_activity_event(const ActivityEvent& ev)
{
    if (!store_) return;
    switch (ev.kind) {
    case ActivityKind::memory_added:
    case ActivityKind::memory_deleted:
        schedule_refresh();
        break;
    case ActivityKind::memory_searched:
        // memory_searched events carry an optional list of returned ids in
        // the detail; we don't pulse-highlight at v1 to keep the panel
        // visually quiet, but trigger a refresh so last_used_at bumps land.
        schedule_refresh();
        break;
    case ActivityKind::tool_result: {
        // Agent-driven add_memory / search_memory don't fire memory_*
        // events directly -- they go through the dispatcher's tool_result.
        // Cheap substring match on the summary keeps us in sync.
        const auto& s = ev.summary;
        if (s.find("add_memory") != std::string::npos
         || s.find("search_memory") != std::string::npos) {
            schedule_refresh();
        }
        break;
    }
    default: break;
    }
}

void MemoryBankPanel::pulse_rows(const std::vector<std::string>&)
{
    // S5.K v1 -- placeholder. A future revision can flash a row background
    // when its id appears in a memory_searched event's detail. Keeping the
    // hook so the panel API matches the stage doc.
}

} // namespace locus
