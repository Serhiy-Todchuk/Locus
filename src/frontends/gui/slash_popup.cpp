#include "slash_popup.h"

#include <wx/display.h>

#include <algorithm>
#include <cctype>

namespace locus {

static bool contains_ci(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != hay.end();
}

SlashPopup::SlashPopup(wxWindow* parent, std::vector<SlashItem> items)
    : wxPopupTransientWindow(parent, wxBORDER_SIMPLE)
    , all_items_(std::move(items))
{
    list_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(360, 200),
                          0, nullptr, wxLB_SINGLE | wxLB_NEEDED_SB);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(list_, 1, wxEXPAND);
    SetSizer(sizer);

    // Single click on an item accepts.
    list_->Bind(wxEVT_LEFT_UP, &SlashPopup::on_list_left_up, this);
    list_->Bind(wxEVT_MOTION, &SlashPopup::on_list_motion, this);

    // Start with all items visible.
    for (size_t i = 0; i < all_items_.size(); ++i) visible_.push_back(i);
    rebuild();
}

void SlashPopup::rebuild()
{
    list_->Freeze();
    list_->Clear();
    for (auto idx : visible_) {
        const auto& it = all_items_[idx];
        wxString label = "/" + wxString::FromUTF8(it.name);
        if (it.is_tool) label += "  [tool]";
        if (!it.description.empty())
            label += "  -  " + wxString::FromUTF8(it.description);
        list_->Append(label);
    }
    if (!visible_.empty()) list_->SetSelection(0);
    list_->Thaw();
}

bool SlashPopup::apply_filter(const wxString& filter_wx)
{
    std::string filter = filter_wx.ToStdString();
    std::string prev_sel = selected_command();

    visible_.clear();
    // Prefer prefix matches first, then substring matches. Preserves the
    // caller's ordering within each bucket.
    std::vector<size_t> prefix, substr;
    for (size_t i = 0; i < all_items_.size(); ++i) {
        const auto& n = all_items_[i].name;
        if (filter.empty()) {
            prefix.push_back(i);
            continue;
        }
        bool is_prefix = n.size() >= filter.size() &&
            std::equal(filter.begin(), filter.end(), n.begin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                });
        if (is_prefix) prefix.push_back(i);
        else if (contains_ci(n, filter)) substr.push_back(i);
    }
    visible_ = std::move(prefix);
    visible_.insert(visible_.end(), substr.begin(), substr.end());
    rebuild();

    // Restore previous selection if still visible.
    if (!prev_sel.empty()) {
        for (size_t vi = 0; vi < visible_.size(); ++vi) {
            if (all_items_[visible_[vi]].name == prev_sel) {
                list_->SetSelection(static_cast<int>(vi));
                break;
            }
        }
    }
    return !visible_.empty();
}

void SlashPopup::show_anchored(const wxPoint& anchor_screen_pos, int anchor_width)
{
    // Size to content. Win32 wxListBox row height = font height + a few px
    // of padding; computing the client area as (rows * row_h) avoids the
    // stray empty tail row that appears when the box is sized loosely.
    const int row_h = list_->GetCharHeight() + 3;
    const int max_rows = 10;
    const int rows = std::min<int>(static_cast<int>(visible_.size()), max_rows);
    const int list_h = std::max(row_h, rows * row_h);
    const int width  = std::max(anchor_width, 340);

    SetClientSize(width, list_h);
    Layout();

    // Place above the anchor so it overlays the chat history, not below the
    // input (which would fall off the window on the bottom pane).
    wxPoint pos = anchor_screen_pos;
    pos.y -= list_h;

    // Clamp to display bounds so the popup is always fully visible.
    int disp_idx = wxDisplay::GetFromPoint(anchor_screen_pos);
    if (disp_idx == wxNOT_FOUND) disp_idx = 0;
    wxRect display = wxDisplay(static_cast<unsigned int>(disp_idx)).GetClientArea();
    if (pos.y < display.GetTop()) pos.y = display.GetTop();
    if (pos.x + width > display.GetRight()) pos.x = display.GetRight() - width;
    if (pos.x < display.GetLeft()) pos.x = display.GetLeft();

    Move(pos);
    Popup();  // nullptr focus target → keep focus where it was (input text ctrl)
}

void SlashPopup::move_up()
{
    int n = static_cast<int>(list_->GetCount());
    if (n == 0) return;
    int sel = list_->GetSelection();
    sel = (sel <= 0) ? n - 1 : sel - 1;
    list_->SetSelection(sel);
}

void SlashPopup::move_down()
{
    int n = static_cast<int>(list_->GetCount());
    if (n == 0) return;
    int sel = list_->GetSelection();
    sel = (sel < 0 || sel >= n - 1) ? 0 : sel + 1;
    list_->SetSelection(sel);
}

std::string SlashPopup::selected_command() const
{
    int sel = list_->GetSelection();
    if (sel < 0 || static_cast<size_t>(sel) >= visible_.size()) return {};
    return all_items_[visible_[sel]].name;
}

bool SlashPopup::has_selection() const
{
    return list_->GetSelection() >= 0;
}

void SlashPopup::on_list_left_up(wxMouseEvent& evt)
{
    int item = list_->HitTest(evt.GetPosition());
    if (item != wxNOT_FOUND) {
        list_->SetSelection(item);
        if (on_accept) {
            auto cmd = selected_command();
            if (!cmd.empty()) on_accept(cmd);
        }
    }
    // Don't Skip: we've handled the click. The owner of on_accept will Dismiss().
}

void SlashPopup::on_list_motion(wxMouseEvent& evt)
{
    // Follow the mouse: highlight the row under the cursor without requiring
    // a click. Matches common autocomplete UX.
    int item = list_->HitTest(evt.GetPosition());
    if (item != wxNOT_FOUND && item != list_->GetSelection())
        list_->SetSelection(item);
    evt.Skip();
}

void SlashPopup::OnDismiss()
{
    if (on_dismiss) on_dismiss();
}

} // namespace locus
