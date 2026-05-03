#include "file_tree_panel.h"

#include <spdlog/spdlog.h>

#include <wx/artprov.h>
#include <wx/menu.h>

#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace locus {

// Sentinel child used to show the expand arrow before lazy-loading.
static const char* k_dummy_child = "__locus_dummy__";

// Tree item data that stores the relative file path.
class TreePathData : public wxTreeItemData {
public:
    explicit TreePathData(const std::string& path) : path_(path) {}
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

wxBEGIN_EVENT_TABLE(FileTreePanel, wxPanel)
wxEND_EVENT_TABLE()

FileTreePanel::FileTreePanel(wxWindow* parent, IndexQuery& query,
                             const std::string& workspace_root,
                             FileSelectedCallback on_select,
                             AttachCallback       on_attach)
    : wxPanel(parent, wxID_ANY)
    , query_(query)
    , workspace_root_(workspace_root)
    , on_select_(std::move(on_select))
    , on_attach_(std::move(on_attach))
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Index status bar at top.
    stats_label_ = new wxStaticText(this, wxID_ANY, "Files: - | Symbols: - | Headings: -");
    stats_label_->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    auto stats_font = stats_label_->GetFont();
    stats_font.SetPointSize(stats_font.GetPointSize() - 1);
    stats_label_->SetFont(stats_font);
    sizer->Add(stats_label_, 0, wxEXPAND | wxALL, 4);

    // Re-index progress gauge (hidden by default).
    reindex_gauge_ = new wxGauge(this, wxID_ANY, 100,
        wxDefaultPosition, wxSize(-1, 8), wxGA_HORIZONTAL | wxGA_SMOOTH);
    reindex_gauge_->Pulse();
    reindex_gauge_->Hide();
    sizer->Add(reindex_gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

    // Tree control.
    tree_ = new wxTreeCtrl(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT | wxTR_LINES_AT_ROOT);

    create_image_list();
    sizer->Add(tree_, 1, wxEXPAND);

    SetSizer(sizer);

    // Bind events.
    tree_->Bind(wxEVT_TREE_ITEM_EXPANDING, &FileTreePanel::on_item_expanding, this);
    tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, &FileTreePanel::on_item_activated, this);
    tree_->Bind(wxEVT_TREE_SEL_CHANGED, &FileTreePanel::on_selection_changed, this);
    tree_->Bind(wxEVT_TREE_ITEM_MENU,    &FileTreePanel::on_item_menu,        this);

    rebuild();
}

void FileTreePanel::create_image_list()
{
    auto* img_list = new wxImageList(16, 16, true, 5);

    // Use wxArtProvider for standard icons.
    img_list->Add(wxArtProvider::GetBitmap(wxART_FOLDER,      wxART_LIST, wxSize(16, 16)));  // folder_closed
    img_list->Add(wxArtProvider::GetBitmap(wxART_FOLDER_OPEN, wxART_LIST, wxSize(16, 16)));  // folder_open
    img_list->Add(wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_LIST, wxSize(16, 16)));  // file_generic
    img_list->Add(wxArtProvider::GetBitmap(wxART_EXECUTABLE_FILE, wxART_LIST, wxSize(16, 16))); // file_code
    img_list->Add(wxArtProvider::GetBitmap(wxART_REPORT_VIEW, wxART_LIST, wxSize(16, 16)));  // file_doc

    tree_->AssignImageList(img_list);  // tree takes ownership
}

static int icon_for_file(const std::string& ext)
{
    // Code file extensions.
    static const char* code_exts[] = {
        ".cpp", ".h", ".c", ".hpp", ".cc", ".cxx",
        ".py", ".js", ".ts", ".java", ".go", ".rs",
        ".cs", ".lua", ".rb", ".swift", ".kt", ".zig",
        ".cmake", ".json", ".yaml", ".yml", ".toml",
        nullptr
    };
    // Document extensions.
    static const char* doc_exts[] = {
        ".md", ".txt", ".rst", ".tex", ".html", ".htm",
        ".pdf", ".docx", ".xlsx",
        nullptr
    };

    for (auto* p = code_exts; *p; ++p)
        if (ext == *p) return FileTreePanel::file_code;
    for (auto* p = doc_exts; *p; ++p)
        if (ext == *p) return FileTreePanel::file_doc;

    return FileTreePanel::file_generic;
}

void FileTreePanel::rebuild()
{
    tree_->Freeze();
    tree_->DeleteAllItems();

    auto root = tree_->AddRoot("root");
    populate_children(root, "");

    tree_->Thaw();
    spdlog::trace("FileTreePanel: tree rebuilt");
}

void FileTreePanel::populate_children(wxTreeItemId parent, const std::string& dir_path)
{
    auto entries = query_.list_directory(dir_path, 0);

    // Sort: directories first, then alphabetical.
    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.is_directory != b.is_directory)
            return a.is_directory > b.is_directory;
        return a.path < b.path;
    });

    for (auto& entry : entries) {
        // Extract just the filename from the path.
        auto pos = entry.path.find_last_of('/');
        std::string name = (pos != std::string::npos)
            ? entry.path.substr(pos + 1)
            : entry.path;

        if (entry.is_directory) {
            auto item = tree_->AppendItem(parent, wxString::FromUTF8(name),
                                          folder_closed, folder_open);
            tree_->SetItemData(item, new TreePathData(entry.path));
            // Add dummy child so the expand arrow shows.
            tree_->AppendItem(item, k_dummy_child);
        } else {
            int icon = icon_for_file(entry.ext);
            auto item = tree_->AppendItem(parent, wxString::FromUTF8(name),
                                          icon, icon);
            tree_->SetItemData(item, new TreePathData(entry.path));
        }
    }
}

void FileTreePanel::on_item_expanding(wxTreeEvent& evt)
{
    auto item = evt.GetItem();
    if (!item.IsOk()) return;

    // Check if this node has only the dummy child (needs lazy-load).
    wxTreeItemIdValue cookie;
    auto first = tree_->GetFirstChild(item, cookie);
    if (first.IsOk() && tree_->GetItemText(first) == k_dummy_child) {
        tree_->DeleteChildren(item);

        auto* data = dynamic_cast<TreePathData*>(tree_->GetItemData(item));
        if (data) {
            populate_children(item, data->path());
        }
    }
}

void FileTreePanel::on_item_activated(wxTreeEvent& evt)
{
    auto item = evt.GetItem();
    if (!item.IsOk()) return;

    // Directory: toggle expansion.
    if (tree_->ItemHasChildren(item)) {
        tree_->Toggle(item);
        return;
    }

    auto* data = dynamic_cast<TreePathData*>(tree_->GetItemData(item));
    if (!data) return;

    // File: open with the OS-default application; also notify on_select_ so
    // the status bar reflects what the user just acted on.
    if (on_select_) on_select_(data->path());
    open_with_os(data->path());
}

void FileTreePanel::on_selection_changed(wxTreeEvent& /*evt*/)
{
    // Future: could show file details in the right panel.
}

// ---------------------------------------------------------------------------
// Right-click context menu
// ---------------------------------------------------------------------------

void FileTreePanel::on_item_menu(wxTreeEvent& evt)
{
    auto item = evt.GetItem();
    if (!item.IsOk()) return;

    // Make sure the item is selected so the user sees what they're acting on.
    tree_->SelectItem(item);

    auto* data = dynamic_cast<TreePathData*>(tree_->GetItemData(item));
    if (!data) return;

    const std::string rel_path = data->path();
    const bool is_dir = tree_->ItemHasChildren(item);

    enum { ID_OPEN = wxID_HIGHEST + 1, ID_ATTACH, ID_SHOW };

    wxMenu menu;
    menu.Append(ID_OPEN, is_dir ? "Open Folder" : "Open");
    if (!is_dir)
        menu.Append(ID_ATTACH, "Attach to context");
    menu.AppendSeparator();
    menu.Append(ID_SHOW, "Show in Explorer");

    // Disable Attach if the host hasn't wired a handler.
    if (!is_dir && !on_attach_)
        menu.Enable(ID_ATTACH, false);

    int picked = GetPopupMenuSelectionFromUser(menu, evt.GetPoint());
    switch (picked) {
    case ID_OPEN:
        open_with_os(rel_path);
        break;
    case ID_ATTACH:
        if (on_attach_) on_attach_(rel_path);
        break;
    case ID_SHOW:
        show_in_explorer(rel_path);
        break;
    default:
        break;  // wxID_NONE — user dismissed the menu
    }
}

// ---------------------------------------------------------------------------
// Shell integration
// ---------------------------------------------------------------------------

void FileTreePanel::open_with_os(const std::string& rel_path)
{
    std::filesystem::path abs = std::filesystem::path(workspace_root_) / rel_path;
    abs = abs.lexically_normal();
    abs.make_preferred();
#ifdef _WIN32
    std::wstring w = abs.wstring();
    HINSTANCE r = ShellExecuteW(nullptr, L"open", w.c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        spdlog::warn("FileTreePanel: ShellExecute open failed for '{}' (code {})",
                     abs.string(), reinterpret_cast<INT_PTR>(r));
    }
#else
    spdlog::warn("FileTreePanel: open_with_os not implemented on this platform");
    (void)abs;
#endif
}

void FileTreePanel::show_in_explorer(const std::string& rel_path)
{
    std::filesystem::path abs = std::filesystem::path(workspace_root_) / rel_path;
    abs = abs.lexically_normal();
    abs.make_preferred();  // forward slashes -> backslashes on Win32
#ifdef _WIN32
    // SHOpenFolderAndSelectItems is the recommended API for this — it handles
    // path normalization, quoting, and UNC paths internally. Passing a string
    // via `explorer.exe /select,...` is brittle: mixed separators cause it to
    // silently open the user's Documents folder instead.
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(abs.wstring().c_str());
    if (!pidl) {
        spdlog::warn("FileTreePanel: ILCreateFromPathW failed for '{}'",
                     abs.string());
        return;
    }
    HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    if (FAILED(hr)) {
        spdlog::warn("FileTreePanel: SHOpenFolderAndSelectItems failed for '{}' (hr=0x{:x})",
                     abs.string(), static_cast<unsigned>(hr));
    }
#else
    spdlog::warn("FileTreePanel: show_in_explorer not implemented on this platform");
    (void)abs;
#endif
}

void FileTreePanel::set_index_stats(int files, int symbols, int headings)
{
    stats_label_->SetLabel(wxString::Format(
        "Files: %d | Symbols: %d | Headings: %d",
        files, symbols, headings));
}

void FileTreePanel::set_reindex_active(bool active)
{
    if (active) {
        reindex_gauge_->Show();
        reindex_gauge_->Pulse();
    } else {
        reindex_gauge_->Hide();
    }
    GetSizer()->Layout();
}

void FileTreePanel::set_embedding_progress(int done, int total)
{
    if (total <= 0) {
        reindex_gauge_->Hide();
    } else if (done >= total) {
        reindex_gauge_->Hide();
        stats_label_->SetLabel(stats_label_->GetLabel() +
                               wxString::Format(" | Vec: %d", total));
    } else {
        reindex_gauge_->Show();
        reindex_gauge_->SetRange(total);
        reindex_gauge_->SetValue(done);
    }
    GetSizer()->Layout();
}

} // namespace locus
