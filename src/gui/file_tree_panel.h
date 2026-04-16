#pragma once

#include "../index_query.h"

#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>

#include <functional>
#include <string>

namespace locus {

// File tree panel for the left sidebar.
// Lazy-loads directory contents from IndexQuery::list_directory().
// Fires a callback when the user selects a file.
class FileTreePanel : public wxPanel {
public:
    using FileSelectedCallback = std::function<void(const std::string& path)>;

    FileTreePanel(wxWindow* parent, IndexQuery& query,
                  const std::string& workspace_root,
                  FileSelectedCallback on_select = {});

    // Rebuild the tree from scratch (e.g. after workspace change or re-index).
    void rebuild();

    // Update the index status labels.
    void set_index_stats(int files, int symbols, int headings);

    // Show/hide the re-index progress gauge.
    void set_reindex_active(bool active);

    // Update embedding progress (done/total chunks).
    void set_embedding_progress(int done, int total);

private:
    void create_image_list();
    void populate_children(wxTreeItemId parent, const std::string& dir_path);

    void on_item_expanding(wxTreeEvent& evt);
    void on_item_activated(wxTreeEvent& evt);
    void on_selection_changed(wxTreeEvent& evt);

    IndexQuery& query_;
    std::string workspace_root_;
    FileSelectedCallback on_select_;

    wxTreeCtrl*   tree_     = nullptr;
    wxStaticText* stats_label_ = nullptr;
    wxGauge*      reindex_gauge_ = nullptr;

    wxDECLARE_EVENT_TABLE();

public:
    // Image list indices (public so free helper functions can use them).
    enum Icon { folder_closed = 0, folder_open, file_generic, file_code, file_doc };
};

} // namespace locus
