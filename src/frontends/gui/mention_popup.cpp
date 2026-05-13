#include "mention_popup.h"

#include <wx/display.h>

#include <algorithm>
#include <cctype>

namespace locus {

namespace {

std::string basename_of(const std::string& path)
{
    auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool contains_ci(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != hay.end();
}

bool starts_with_ci(const std::string& hay, const std::string& needle)
{
    if (needle.size() > hay.size()) return false;
    return std::equal(needle.begin(), needle.end(), hay.begin(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
}

} // namespace

MentionPopup::MentionPopup(wxWindow* parent, std::vector<std::string> file_paths)
    : wxPopupTransientWindow(parent, wxBORDER_SIMPLE)
    , all_paths_(std::move(file_paths))
{
    list_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(420, 200),
                          0, nullptr, wxLB_SINGLE | wxLB_NEEDED_SB);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(list_, 1, wxEXPAND);
    SetSizer(sizer);

    list_->Bind(wxEVT_LEFT_UP, &MentionPopup::on_list_left_up, this);
    list_->Bind(wxEVT_MOTION, &MentionPopup::on_list_motion, this);

    for (size_t i = 0; i < all_paths_.size(); ++i) visible_.push_back(i);
    rebuild();
}

void MentionPopup::rebuild()
{
    list_->Freeze();
    list_->Clear();
    for (auto idx : visible_) {
        const auto& full = all_paths_[idx];
        auto base = basename_of(full);
        wxString label = wxString::FromUTF8(base);
        if (base != full) {
            label += "    ";
            label += wxString::FromUTF8(full);
        }
        list_->Append(label);
    }
    if (!visible_.empty()) list_->SetSelection(0);
    list_->Thaw();
}

bool MentionPopup::apply_filter(const wxString& filter_wx)
{
    std::string filter = filter_wx.ToStdString();
    std::string prev_sel = selected_path();

    visible_.clear();
    // Ranking buckets (best first): basename prefix > path-prefix > basename
    // substring > path substring. Order within a bucket follows the caller's
    // original list order so deterministic suites keep their layout.
    std::vector<size_t> base_prefix, path_prefix, base_sub, path_sub;
    for (size_t i = 0; i < all_paths_.size(); ++i) {
        const auto& full = all_paths_[i];
        if (filter.empty()) {
            base_prefix.push_back(i);
            continue;
        }
        std::string base = basename_of(full);
        if (starts_with_ci(base, filter))      base_prefix.push_back(i);
        else if (starts_with_ci(full, filter)) path_prefix.push_back(i);
        else if (contains_ci(base, filter))    base_sub.push_back(i);
        else if (contains_ci(full, filter))    path_sub.push_back(i);
    }
    visible_ = std::move(base_prefix);
    visible_.insert(visible_.end(), path_prefix.begin(), path_prefix.end());
    visible_.insert(visible_.end(), base_sub.begin(),    base_sub.end());
    visible_.insert(visible_.end(), path_sub.begin(),    path_sub.end());

    // Cap the visible list -- 200 rows in a popup is unusable.
    constexpr size_t k_max_visible = 50;
    if (visible_.size() > k_max_visible) visible_.resize(k_max_visible);

    rebuild();

    if (!prev_sel.empty()) {
        for (size_t vi = 0; vi < visible_.size(); ++vi) {
            if (all_paths_[visible_[vi]] == prev_sel) {
                list_->SetSelection(static_cast<int>(vi));
                break;
            }
        }
    }
    return !visible_.empty();
}

void MentionPopup::show_anchored(const wxPoint& anchor_screen_pos, int anchor_width)
{
    const int row_h = list_->GetCharHeight() + 3;
    const int max_rows = 10;
    const int rows = std::min<int>(static_cast<int>(visible_.size()), max_rows);
    const int list_h = std::max(row_h, rows * row_h);
    const int width  = std::max(anchor_width, 380);

    SetClientSize(width, list_h);
    Layout();

    wxPoint pos = anchor_screen_pos;
    pos.y -= list_h;

    int disp_idx = wxDisplay::GetFromPoint(anchor_screen_pos);
    if (disp_idx == wxNOT_FOUND) disp_idx = 0;
    wxRect display = wxDisplay(static_cast<unsigned int>(disp_idx)).GetClientArea();
    if (pos.y < display.GetTop()) pos.y = display.GetTop();
    if (pos.x + width > display.GetRight()) pos.x = display.GetRight() - width;
    if (pos.x < display.GetLeft()) pos.x = display.GetLeft();

    Move(pos);
    Popup();
}

void MentionPopup::move_up()
{
    int n = static_cast<int>(list_->GetCount());
    if (n == 0) return;
    int sel = list_->GetSelection();
    sel = (sel <= 0) ? n - 1 : sel - 1;
    list_->SetSelection(sel);
}

void MentionPopup::move_down()
{
    int n = static_cast<int>(list_->GetCount());
    if (n == 0) return;
    int sel = list_->GetSelection();
    sel = (sel < 0 || sel >= n - 1) ? 0 : sel + 1;
    list_->SetSelection(sel);
}

std::string MentionPopup::selected_path() const
{
    int sel = list_->GetSelection();
    if (sel < 0 || static_cast<size_t>(sel) >= visible_.size()) return {};
    return all_paths_[visible_[sel]];
}

bool MentionPopup::has_selection() const
{
    return list_->GetSelection() >= 0;
}

void MentionPopup::on_list_left_up(wxMouseEvent& evt)
{
    int item = list_->HitTest(evt.GetPosition());
    if (item != wxNOT_FOUND) {
        list_->SetSelection(item);
        if (on_accept) {
            auto p = selected_path();
            if (!p.empty()) on_accept(p);
        }
    }
}

void MentionPopup::on_list_motion(wxMouseEvent& evt)
{
    int item = list_->HitTest(evt.GetPosition());
    if (item != wxNOT_FOUND && item != list_->GetSelection())
        list_->SetSelection(item);
    evt.Skip();
}

void MentionPopup::OnDismiss()
{
    if (on_dismiss) on_dismiss();
}

} // namespace locus
