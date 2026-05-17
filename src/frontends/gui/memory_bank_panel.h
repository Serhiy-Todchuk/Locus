#pragma once

#include "../../core/activity_event.h"
#include "../../core/memory_filter.h"
#include "../../core/memory_store.h"

#include <wx/wx.h>
#include <wx/dataview.h>
#include <wx/splitter.h>
#include <wx/timer.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace locus {

// S5.K -- dockable Memory Bank panel.
//
// Top-level surface for browsing, filtering, editing, and curating the
// workspace's memory bank. Pure view over `MemoryStore` -- the store is the
// source of truth on disk + indexes; this panel renders a snapshot and
// dispatches mutations back to the store.
//
// Live updates: the panel listens to `ActivityEvent`s of kinds
// `memory_added` / `memory_searched` / `memory_deleted` plus any
// `tool_result` for the `add_memory` / `search_memory` tools (the agent
// path doesn't fire memory_* events itself, only tool_result, so we listen
// for both). A 200 ms debounced refresh coalesces bursts.
class MemoryBankPanel : public wxPanel {
public:
    // `store` must outlive the panel. Pass nullptr to leave the panel in a
    // disabled "memory bank is off" state -- the panel still constructs so
    // the AUI pane slot remains stable across capability toggles.
    MemoryBankPanel(wxWindow* parent, MemoryStore* store);

    // Force a full re-read from the store. Cheap on v1 sizes (max_entries
    // 200 by default).
    void refresh();

    // Activity-event hook. Filters internally for the kinds it cares about
    // (memory_added / memory_searched / memory_deleted, tool_result with
    // add_memory / search_memory in summary).
    void on_activity_event(const ActivityEvent& ev);

    // Swap the underlying store (e.g. after a workspace re-open or a
    // capability toggle that recreates the store). Pass nullptr to detach.
    void set_store(MemoryStore* store);

private:
    // -- layout -------------------------------------------------------------
    void build_toolbar(wxWindow* parent, wxSizer* sizer);
    void build_list(wxWindow* parent, wxSizer* sizer);
    void build_detail(wxWindow* parent, wxSizer* sizer);

    // -- event handlers -----------------------------------------------------
    void on_search_changed(wxCommandEvent& evt);
    void on_filter_changed(wxCommandEvent& evt);
    void on_clear_filters(wxCommandEvent& evt);
    void on_show_deleted_toggled(wxCommandEvent& evt);

    void on_row_selected(wxDataViewEvent& evt);
    void on_row_activated(wxDataViewEvent& evt);
    void on_row_right_click(wxDataViewEvent& evt);

    void on_delete_selected(wxCommandEvent& evt);
    void on_pin_toggle_selected(wxCommandEvent& evt);
    void on_tag_selected(wxCommandEvent& evt);
    void on_save_detail(wxCommandEvent& evt);
    void on_restore_selected(wxCommandEvent& evt);

    void on_debounce_timer(wxTimerEvent& evt);

    // -- internal mutations + render ---------------------------------------
    void schedule_refresh();  // debounced
    void render_list();
    void render_detail_for(const std::string& id);
    void clear_detail();
    void populate_tag_filter();
    void apply_capability_state();

    // Helpers for selection extraction.
    std::vector<std::string>      selected_ids() const;
    std::optional<std::string>    single_selected_id() const;

    // Bulk-op confirmation helpers.
    bool confirm_bulk_delete(const std::vector<std::string>& ids);

    // Show / pulse animation on rows touched by a memory_searched event.
    void pulse_rows(const std::vector<std::string>& ids);

    // -- state --------------------------------------------------------------
    MemoryStore*                              store_       = nullptr;
    bool                                      show_deleted_ = false;

    // Top toolbar widgets.
    wxTextCtrl*       search_box_     = nullptr;
    wxChoice*         source_choice_  = nullptr;
    wxChoice*         tag_choice_     = nullptr;
    wxCheckBox*       pinned_only_cb_ = nullptr;
    wxCheckBox*       show_deleted_cb_= nullptr;
    wxButton*         clear_btn_      = nullptr;
    wxButton*         delete_btn_     = nullptr;
    wxButton*         pin_btn_        = nullptr;
    wxButton*         tag_btn_        = nullptr;
    wxButton*         restore_btn_    = nullptr;

    // List + detail.
    wxSplitterWindow*       splitter_     = nullptr;
    wxDataViewListCtrl*     list_         = nullptr;
    wxPanel*                detail_panel_ = nullptr;
    wxStaticText*           detail_meta_  = nullptr;
    wxTextCtrl*             detail_content_ = nullptr;
    wxTextCtrl*             detail_tags_  = nullptr;
    wxCheckBox*             detail_pinned_= nullptr;
    wxButton*               save_btn_     = nullptr;

    // Current snapshot from the store (filtered + sorted). Indexed by
    // row -- the wxDataViewListCtrl is in non-virtual mode here (cheap on
    // v1 size, simpler than virtual). Sister table for deleted entries
    // when show_deleted_ is true.
    std::vector<MemoryStore::Entry>   rows_;
    std::vector<DeletedEntryStub>     deleted_rows_;
    std::string                       current_detail_id_;

    // Debounce timer for search-as-you-type + activity bursts.
    wxTimer                           debounce_timer_;
};

} // namespace locus
